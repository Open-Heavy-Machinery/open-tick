/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file flywheel.c
 * @brief otk-flywheel: the smallest machine that uses all of open-tick v0.1.
 *
 *     otk-flywheel examples/flywheel/step.csv [--driver free-run|paced] [--trace out.csv] [--quiet]
 *                  [--frames out.frames.csv] [--seed N]
 *                  [--criteria examples/flywheel/step.accept.csv [--verdicts out.verdicts.csv]]
 *     otk-flywheel examples/flywheel/hard60.csv --driver hard --dev /dev/ttyUSB1 --trace out.csv
 *                  [--H 16] [--can vcan0 [--frames out.frames.csv]] [--budget out.budget.csv]
 *                  [--fidelity examples/flywheel/fidelity.csv --bridge-fidelity gateware/tang_nano_20k/fidelity.csv]
 *                  [--bitstream-md5 MD5] [--commit SHA] [--allow-reused-bridge]
 *
 * A DC motor spins a flywheel; a toy speed controller (the "device under
 * test") holds the speed the operator asks for. They talk CAN through
 * open-tick's in-memory bridge, exactly as they would over a bus:
 *
 *     machine -> DUT   0x100 SPEED    every 10 ms   uint16 LE, rpm
 *     machine -> DUT   0x101 TARGET   every 20 ms   uint16 LE, rpm (operator panel)
 *     DUT -> machine   0x200 DUTY     every 10 ms   uint8, 0.5 %/bit
 *
 * The machine's frames reach the DUT through a link (opentick/link.h) named
 * `speed`, whose faults apply to SPEED (0x100) only. The scenario drives them
 * with the signals fault.link.speed.* (examples/flywheel/delay.csv); with no
 * fault set the link is transparent.
 *
 * --frames writes every frame of the run (docs/frame-log-format.md): SPEED and
 * TARGET through the speed link, with what it did to each; DUTY, which reaches
 * the machine through a plain queue, with no link name. In memory both times
 * are simulated (bus_clock=sim): t_bus - t_s is exactly the link's delay.
 *
 * --seed seeds the speed link's PRNG, which draws the jitter
 * (fault.link.speed.jitter_ms; examples/flywheel/jitter.csv). The default is 1.
 * The same seed gives the same run, byte for byte; another seed gives other
 * draws. Without jitter the seed changes nothing.
 *
 * --driver hard steps the flywheel on a signal-level bridge's clock
 * (opentick/host/hard_loop.h, docs/bridge-protocol.md): the plant runs H steps
 * ahead of the bridge, and every step's setpoint makes the bridge's rotary
 * pattern turn at the flywheel's speed and its analog output 0 show that
 * speed as a fraction of 3000 rpm. The controller still runs in memory. With
 * --can, every SPEED frame also goes out on SocketCAN when the bridge begins
 * its step (§5), and --frames then logs those frames on the bridge's clock
 * (bus_clock=bridge), read back with the kernel's timestamps. Every hard run
 * writes its budget file next to the trace (out.budget.csv for out.csv), or
 * where --budget says: what was declared and what was measured. Its exit
 * code is 0 for ok, 3 for degraded (an underrun, a late setpoint) and 4 for
 * aborted (the link timed out).
 *
 * --fidelity <machine.csv> --bridge-fidelity <bridge.csv> checks, before the
 * bridge is opened, that the machine's fidelity contract is within what the
 * bridge guarantees (docs/bridge-protocol.md §7; the Tang Nano 20K's is
 * gateware/tang_nano_20k/fidelity.csv). The bridge's lead and DUT-to-plant
 * delay are this run's: H x step, and that plus the declared round trip. A
 * refusal names the signal and the column, exits 5 and leaves the bridge
 * untouched.
 *
 * This is an example, not an application: the plant is a first-order sketch
 * and the message set is made up. It exists to show the pieces together:
 * a signal table, a scenario, the player, CAN queues, a link, a trace and a
 * driver.
 */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opentick/can.h>
#include <opentick/fidelity.h>
#include <opentick/framelog.h>
#include <opentick/host/driver.h>
#include <opentick/host/hard_loop.h>
#include <opentick/host/socketcan.h>
#include <opentick/link.h>
#include <opentick/oracle.h>
#include <opentick/player.h>
#include <opentick/scenario.h>
#include <opentick/trace.h>

#define STEP_MS 10U
#define ID_SPEED 0x100U
#define ID_TARGET 0x101U
#define ID_DUTY 0x200U
#define PI 3.14159265358979323846
#define LINK_SEED_DEFAULT 1U /* the speed link's PRNG (jitter); --seed changes it */
#define TICK_HZ 27000000U    /* the Tang Nano 20K's oscillator */
#define STEP_TICKS (STEP_MS * (TICK_HZ / 1000U))
#define FULL_SCALE_RPM 3000.0 /* analog output 0: the speed as a fraction of this */
#define EXIT_DEGRADED 3
#define EXIT_ABORTED 4
#define CYCLE_NS 6000000U /* the BL616's forwarding cycle, as the hard loop's guess (§4.1) */
#define EXIT_FIDELITY 5 /* --fidelity: the machine requires more than the bridge guarantees */

/* ---- Signals the scenario can set ---------------------------------------- */

enum {
    SIG_TARGET_RPM,
    SIG_LOAD_NM,
    SIG_FAULT_SPEED_LOST,
    SIG_LINK_SPEED, /* the six fault.link.speed.* signals */
    SIG_COUNT = SIG_LINK_SPEED + OTK_LINK_SIGNAL_COUNT
};
static const char *const SIGNAL_NAMES[SIG_COUNT] = {
    [SIG_TARGET_RPM] = "target_rpm",
    [SIG_LOAD_NM] = "load_nm",
    [SIG_FAULT_SPEED_LOST] = "fault.speed_lost",
    [SIG_LINK_SPEED] = OTK_LINK_SIGNAL_NAMES("speed"),
};
static const OtkSignalTable SIGNALS = { SIGNAL_NAMES, SIG_COUNT };

/* ---- The machine: flywheel, motor, operator panel ------------------------ */

typedef struct {
    /* signals */
    double target_rpm;
    double load_nm;
    int speed_lost;
    OtkLinkFaults speed_link; /* fault.link.speed.* */
    /* state */
    uint64_t t_ms;
    double omega;       /* rad/s */
    double duty_cmd;    /* last received, 0..1 */
    uint64_t duty_ms;   /* when it arrived */
    int duty_seen;
    double duty;        /* applied: zero once the command is stale */
} Machine;

static const double J_KGM2 = 0.002;
static const double STALL_NM = 1.5;           /* motor torque at 0 rpm, full duty */
static const double NO_LOAD_RAD_S = 3000.0 * PI / 30.0;
static const uint64_t DUTY_TIMEOUT_MS = 100U; /* a silent controller means zero duty */

static void machine_apply(void *ctx, OtkSignalId signal, double value) {
    Machine *m = ctx;
    switch (signal) {
    case SIG_TARGET_RPM:
        m->target_rpm = value;
        break;
    case SIG_LOAD_NM:
        m->load_nm = value;
        break;
    case SIG_FAULT_SPEED_LOST:
        m->speed_lost = value != 0.0;
        break;
    default:
        (void)otk_link_signal(&m->speed_link, SIG_LINK_SPEED, signal, value);
        break;
    }
}

static double rpm_of(const Machine *m) {
    return m->omega * 30.0 / PI;
}

static void machine_rx(Machine *m, const OtkCanFrame *f) {
    if (f->id == ID_DUTY && f->len >= 1U && (f->flags & OTK_CAN_EXTENDED) == 0U) {
        m->duty_cmd = (double)f->data[0] * 0.005;
        m->duty_ms = m->t_ms;
        m->duty_seen = 1;
    }
}

static OtkCanFrame u16_frame(uint32_t id, double value) {
    OtkCanFrame f;
    memset(&f, 0, sizeof(f));
    const double v = (value < 0.0) ? 0.0 : (value > 65535.0) ? 65535.0 : value;
    const uint16_t raw = (uint16_t)(v + 0.5);
    f.id = id;
    f.len = 2U;
    f.data[0] = (uint8_t)(raw & 0xFFU);
    f.data[1] = (uint8_t)(raw >> 8);
    return f;
}

/* One step: physics, then the frames the machine sends. */
static void machine_step(Machine *m, OtkCanQueue *to_dut) {
    const double dt = STEP_MS / 1000.0;
    const int fresh = m->duty_seen && (m->t_ms - m->duty_ms) <= DUTY_TIMEOUT_MS;
    m->duty = fresh ? m->duty_cmd : 0.0;

    const double motor_nm = STALL_NM * (m->duty - m->omega / NO_LOAD_RAD_S);
    const double friction_nm = (m->omega > 0.0) ? 0.02 : 0.0;
    m->omega += dt * (motor_nm - friction_nm - m->load_nm) / J_KGM2;
    if (m->omega < 0.0) {
        m->omega = 0.0; /* the load brakes; it does not spin the wheel backwards */
    }
    m->t_ms += STEP_MS;

    if (!m->speed_lost) {
        const OtkCanFrame f = u16_frame(ID_SPEED, rpm_of(m));
        (void)otk_can_queue_push(to_dut, &f);
    }
    if (m->t_ms % 20U == 0U) {
        const OtkCanFrame f = u16_frame(ID_TARGET, m->target_rpm);
        (void)otk_can_queue_push(to_dut, &f);
    }
}

/* ---- The device under test: a toy PI speed controller -------------------- */

typedef enum { DUT_WAIT, DUT_RUN, DUT_FAULT } DutState;
static const char *const DUT_STATE_NAMES[] = { "WAIT", "RUN", "FAULT" };

typedef struct {
    DutState state;
    double speed_rpm;
    uint64_t speed_ms;
    int speed_seen;
    double target_rpm;
    double integral;
    double duty;
} Dut;

static uint16_t u16_le(const OtkCanFrame *f) {
    return (uint16_t)(f->data[0] | (f->data[1] << 8));
}

static void dut_rx(Dut *d, const OtkCanFrame *f, uint64_t now_ms) {
    if ((f->flags & OTK_CAN_EXTENDED) != 0U || f->len < 2U) {
        return;
    }
    if (f->id == ID_SPEED) {
        d->speed_rpm = u16_le(f);
        d->speed_ms = now_ms;
        d->speed_seen = 1;
    } else if (f->id == ID_TARGET) {
        d->target_rpm = u16_le(f);
    }
}

static void dut_step(Dut *d, uint64_t now_ms, OtkCanQueue *to_machine) {
    const int fresh = d->speed_seen && (now_ms - d->speed_ms) <= 100U;
    if (!fresh) {
        d->state = d->speed_seen ? DUT_FAULT : DUT_WAIT; /* safe state: no drive */
        d->integral = 0.0;
        d->duty = 0.0;
    } else {
        d->state = DUT_RUN;
        const double err = d->target_rpm - d->speed_rpm;
        const double p = 0.002 * err;
        const double i_next = d->integral + 0.004 * err * (STEP_MS / 1000.0);
        const double out = p + i_next;
        if (out >= 0.0 && out <= 1.0) {
            d->integral = i_next; /* anti-windup: integrate only while not saturated */
        }
        d->duty = fmin(1.0, fmax(0.0, p + d->integral));
    }
    OtkCanFrame f;
    memset(&f, 0, sizeof(f));
    f.id = ID_DUTY;
    f.len = 1U;
    f.data[0] = (uint8_t)(d->duty / 0.005 + 0.5);
    (void)otk_can_queue_push(to_machine, &f);
}

/* ---- Trace ---------------------------------------------------------------- */

static const OtkTraceColumn COLUMNS[] = {
    { "t_s", OTK_COL_REAL, 3 },      { "target_rpm", OTK_COL_REAL, 1 }, { "load_nm", OTK_COL_REAL, 3 },
    { "rpm", OTK_COL_REAL, 2 },      { "duty_pct", OTK_COL_REAL, 1 },   { "dut_state", OTK_COL_TEXT, 0 },
};

static void trace_row(OtkTrace *t, const Machine *m, const Dut *d) {
    otk_trace_real(t, (double)m->t_ms / 1000.0);
    otk_trace_real(t, m->target_rpm);
    otk_trace_real(t, m->load_nm);
    otk_trace_real(t, rpm_of(m));
    otk_trace_real(t, 100.0 * m->duty);
    otk_trace_text(t, DUT_STATE_NAMES[d->state]);
    (void)otk_trace_end_row(t);
}

/* ---- Oracle: what the DUT sends, as the machine would see it ------------- */

enum { OBS_DUTY_PCT, OBS_COUNT };
static const OtkOracleObservable OBSERVABLES[OBS_COUNT] = {
    /* The machine treats a duty command older than 100 ms as zero: so does the oracle. */
    [OBS_DUTY_PCT] = { "duty_pct", 100U * OTK_NS_PER_MS, { false, 0.0 } },
};

static size_t decode(void *ctx, const OtkCanFrame *f, OtkOracleUpdate *out, size_t cap) {
    (void)ctx;
    if (cap < 1U || f->id != ID_DUTY || f->len < 1U || (f->flags & OTK_CAN_EXTENDED) != 0U) {
        return 0U;
    }
    out[0] = (OtkOracleUpdate){ OBS_DUTY_PCT, { false, (double)f->data[0] * 0.5 } };
    return 1U;
}

static const OtkOracleTable ORACLE_TABLE = { OBSERVABLES, OBS_COUNT, decode, NULL };

/* ---- Main: the in-memory bridge and a driver ------------------------------ */

static OtkScenario g_scenario;
static OtkOracle g_oracle; /* large: keep it off the stack */

static void usage(void) {
    fprintf(stderr, "usage: otk-flywheel <scenario.csv> [--driver free-run|paced|hard] [--trace <out.csv>] [--quiet]\n"
                    "                   [--criteria <criteria.csv> [--verdicts <out.csv>]] [--frames <out.csv>]\n"
                    "                   [--seed <n>]   seed of the speed link's jitter (default 1)\n"
                    "       hard driver: --dev <tty> [--H <steps>] [--can <if>] [--budget <out.csv>]\n"
                    "                   [--bitstream-md5 <md5>] [--commit <sha>] [--link <name>] [--allow-reused-bridge]\n"
                    "                   [--fidelity <machine.csv> --bridge-fidelity <bridge.csv>]\n");
}

/* ---- Fidelity (--fidelity) ------------------------------------------------- */

static OtkFidelityFile g_fid_machine, g_fid_bridge; /* large: keep them off the stack */

static bool read_fidelity(const char *path, OtkFidelityFile *f) {
    static char text[65536];
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        fprintf(stderr, "otk-flywheel: cannot read %s: %s\n", path, strerror(errno));
        return false;
    }
    const size_t n = fread(text, 1U, sizeof(text), fp);
    const bool too_long = n == sizeof(text);
    fclose(fp);
    if (too_long) {
        fprintf(stderr, "otk-flywheel: %s is longer than %zu bytes\n", path, sizeof(text) - 1U);
        return false;
    }
    OtkFidelityError e;
    if (!otk_fidelity_parse(text, n, f, &e)) {
        fprintf(stderr, "otk-flywheel: %s, line %u%s%s: %s\n", path, e.line, e.column ? ", column " : "",
                e.column ? e.column : "", e.what);
        return false;
    }
    return true;
}

/* 0: within; EXIT_FIDELITY: refused; 2: a file could not be read. */
static int check_fidelity(const char *machine_path, const char *bridge_path, unsigned h, uint64_t rtt_ns) {
    if (!read_fidelity(machine_path, &g_fid_machine) || !read_fidelity(bridge_path, &g_fid_bridge)) {
        return 2;
    }
    /* the lead and the delay are this run's (§7): H steps; and the delay a capture can have (§2) */
    g_fid_bridge.has_lead = g_fid_bridge.has_delay = true;
    g_fid_bridge.lead_ns = (uint64_t)h * STEP_MS * 1000000U;
    g_fid_bridge.dut_to_plant_delay_ns = otk_hard_dut_to_plant_ns(h, (uint64_t)STEP_MS * 1000000U, CYCLE_NS, rtt_ns);
    OtkFidelityResult r;
    char msg[512];
    const OtkFidelityVerdict v = otk_fidelity_check(&g_fid_machine, &g_fid_bridge, &r);
    otk_fidelity_describe(&r, &g_fid_machine, &g_fid_bridge, msg, sizeof(msg));
    fprintf(v == OTK_FIDELITY_OK ? stdout : stderr,
            "otk-flywheel: %s (lead %llu ns, DUT-to-plant delay %llu ns: H = %u steps of %u ms)\n", msg,
            (unsigned long long)g_fid_bridge.lead_ns, (unsigned long long)g_fid_bridge.dut_to_plant_delay_ns, h,
            STEP_MS);
    return v == OTK_FIDELITY_OK ? 0 : EXIT_FIDELITY;
}

/* ---- Frame log (--frames) --------------------------------------------------- */

static OtkFrameLog g_frames;
static FILE *g_frames_fp = NULL;

static void log_frame(OtkNs t, OtkNs t_bus, OtkFrameDir dir, const char *link, const OtkCanFrame *f,
                      OtkFrameFault fault) {
    if (g_frames_fp != NULL) {
        const OtkFrameRow row = { t, t_bus, dir, link, f, fault };
        (void)otk_framelog_frame(&g_frames, &row); /* an error is sticky and reported at the end */
    }
}

/* The machine sends at t: a frame the link loses never reaches the bus. */
static void send_to_dut(OtkLink *link, const OtkCanFrame *f, OtkNs t) {
    OtkLinkInfo info;
    if (!otk_link_push_ex(link, f, t, &info)) {
        log_frame(t, OTK_FRAMELOG_NO_TIME, OTK_FRAME_TO_DUT, "speed", f, otk_frame_fault_from_link(&info));
    }
}

/* What the link delivers to the DUT at now; false when nothing more is due. */
static bool deliver_to_dut(OtkLink *link, OtkNs now, OtkCanFrame *out) {
    OtkLinkInfo info;
    while (otk_link_pop_ex(link, now, out, &info)) {
        const bool lost = info.lost != OTK_LINK_LOST_NONE;
        log_frame(info.pushed, lost ? OTK_FRAMELOG_NO_TIME : now, OTK_FRAME_TO_DUT, "speed", out,
                  otk_frame_fault_from_link(&info));
        if (!lost) {
            return true;
        }
    }
    return false;
}

/* ---- The hard driver (--driver hard) ---------------------------------------- */

/* What a hard run of this example is declared to hold on the Tang Nano 20K's
 * BL616 link (docs/bridge-protocol.md, "Budget of a hard run"). Each run
 * measures every row and writes both. */
static OtkHardBudget declared_budget(void) {
    OtkHardBudget b;
    otk_hard_budget_none(&b);
    b.underruns = 0U;
    b.late = 0U;
    b.rtt_p99_ns = 2200000U;       /* scheduled against the cycle, 1200 us margin (docs/bridge-protocol.md §9) */
    b.mapping_bound_ns = 500000U;  /* 385 us measured at that margin */
    b.realtime_err_ns = 10000U;
    b.can_tx_err_ns = 600000U;     /* the mapping's bound plus the sender's lateness on a quiet host */
    return b;
}

static OtkHardLoop g_hard;
static OtkSocketCan g_can;
static bool g_can_on = false;
static uint32_t g_sent_step[256]; /* bridge steps of the SPEED frames sent, until their echo is read */
static size_t g_sent_head = 0U, g_sent_n = 0U;

typedef struct {
    OtkNs t, t_bus;
    OtkFrameDir dir;
    OtkCanFrame frame;
} HardRow;
static HardRow *g_rows = NULL; /* the frame log is written at the end, once its error is measured */
static size_t g_nrows = 0U, g_caprows = 0U;
static uint64_t *g_tx_err = NULL; /* |t_bus - t| of every SPEED frame read back */
static size_t g_ntx = 0U, g_captx = 0U;
static bool g_hard_oom = false;

static void on_can_sent(void *ctx, const OtkHardCanSent *sent) {
    (void)ctx;
    if (sent->ok && g_sent_n < sizeof(g_sent_step) / sizeof(g_sent_step[0])) {
        g_sent_step[(g_sent_head + g_sent_n) % 256U] = sent->step;
        g_sent_n++;
    }
}

static void *grow(void *p, size_t *cap, size_t n, size_t size) {
    if (n < *cap) {
        return p;
    }
    const size_t c = *cap ? 2U * *cap : 1024U;
    void *q = realloc(p, c * size);
    if (q == NULL) {
        g_hard_oom = true;
        return p;
    }
    *cap = c;
    return q;
}

/* Reads what came back from the bus: our SPEED frames with the kernel's
 * timestamps, and any other node's frames, all mapped onto the bridge. */
static void hard_read_bus(uint32_t step_now) {
    if (!g_can_on) {
        return;
    }
    OtkCanFrame f;
    OtkSocketCanRxInfo info;
    while (otk_socketcan_recv_ex(&g_can, &f, &info) == 1) {
        int64_t bus = 0;
        const bool mapped = info.has_time && otk_hard_loop_bridge_ns_rt(&g_hard, info.time_ns, &bus) && bus >= 0;
        HardRow r;
        r.frame = f;
        r.t_bus = mapped ? (OtkNs)bus : OTK_FRAMELOG_NO_TIME;
        if (info.own) {
            if (g_sent_n == 0U) {
                continue;
            }
            const uint32_t step = g_sent_step[g_sent_head];
            g_sent_head = (g_sent_head + 1U) % 256U;
            g_sent_n--;
            r.t = otk_ns_from_ms((uint64_t)step * STEP_MS);
            r.dir = OTK_FRAME_TO_DUT;
            if (mapped) {
                g_tx_err = grow(g_tx_err, &g_captx, g_ntx, sizeof(uint64_t));
                if (g_ntx < g_captx) {
                    g_tx_err[g_ntx++] = r.t_bus > r.t ? r.t_bus - r.t : r.t - r.t_bus;
                }
            }
        } else {
            r.t = otk_ns_from_ms((uint64_t)step_now * STEP_MS);
            r.dir = OTK_FRAME_FROM_DUT;
        }
        g_rows = grow(g_rows, &g_caprows, g_nrows, sizeof(HardRow));
        if (g_nrows < g_caprows) {
            g_rows[g_nrows++] = r;
        }
    }
}

static int cmp_u64(const void *a, const void *b) {
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static OtkHardCanErr can_err(void) {
    OtkHardCanErr e = { 0U, 0U, 0U, 0U };
    if (g_ntx == 0U) {
        return e;
    }
    qsort(g_tx_err, g_ntx, sizeof(uint64_t), cmp_u64);
    e.n = g_ntx;
    e.p50_ns = g_tx_err[(size_t)ceil(0.50 * (double)g_ntx) - 1U];
    e.p99_ns = g_tx_err[(size_t)ceil(0.99 * (double)g_ntx) - 1U];
    e.max_ns = g_tx_err[g_ntx - 1U];
    return e;
}

static bool write_hard_frames(const char *path) {
    const uint64_t err_ns = otk_hard_loop_bus_err_ns(&g_hard);
    const OtkFrameLogConfig cfg = { otk_ns_from_ms(STEP_MS), OTK_BUS_CLOCK_BRIDGE, err_ns != UINT64_MAX,
                                    err_ns == UINT64_MAX ? 0U : err_ns, "otk-flywheel" };
    FILE *fp = fopen(path, "w");
    OtkFrameLog log;
    bool ok = fp != NULL && otk_framelog_init(&log, fp, &cfg);
    for (size_t i = 0; ok && i < g_nrows; i++) {
        const OtkFrameRow row = { g_rows[i].t, g_rows[i].t_bus, g_rows[i].dir, NULL, &g_rows[i].frame, OTK_FRAME_OK };
        ok = otk_framelog_frame(&log, &row);
    }
    if (fp != NULL && !ok) {
        fprintf(stderr, "otk-flywheel: frame log: %s\n", otk_framelog_error(&log));
    }
    if (fp == NULL || fclose(fp) != 0) {
        ok = false;
    }
    return ok;
}

/* out.csv -> out.budget.csv; anything else gets .budget.csv appended. */
static void budget_path_of(const char *trace, char *out, size_t cap) {
    const size_t n = strlen(trace);
    if (n > 4U && strcmp(trace + n - 4U, ".csv") == 0) {
        snprintf(out, cap, "%.*s.budget.csv", (int)(n - 4U), trace);
    } else {
        snprintf(out, cap, "%s.budget.csv", trace);
    }
}

static OtkBridgeSetpoint setpoint_of(double rpm) {
    OtkBridgeSetpoint sp;
    memset(&sp, 0, sizeof(sp));
    uint32_t inc = 0U;
    if (otk_bridge_phase_inc(rpm, TICK_HZ, &inc)) {
        sp.crank_phase_inc = inc;
    }
    sp.analog[0] = otk_bridge_analog(rpm / FULL_SCALE_RPM);
    return sp;
}

int main(int argc, char **argv) {
    const char *scenario_path = NULL;
    const char *trace_path = NULL;
    const char *criteria_path = NULL;
    uint64_t link_seed = LINK_SEED_DEFAULT;
    const char *verdicts_path = NULL;
    const char *frames_path = NULL;
    OtkDriverKind kind = OTK_DRIVER_FREE_RUN;
    int quiet = 0;
    const char *dev = NULL, *can_if = NULL, *budget_path = NULL, *bitstream_md5 = NULL, *commit = NULL;
    const char *link_name = "bl616-3mbaud";
    const char *fidelity_path = NULL, *bridge_fidelity_path = NULL;
    unsigned hard_h = OTK_HARD_DEFAULT_H;
    bool allow_reused = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dev") == 0 && i + 1 < argc) {
            dev = argv[++i];
        } else if (strcmp(argv[i], "--can") == 0 && i + 1 < argc) {
            can_if = argv[++i];
        } else if (strcmp(argv[i], "--budget") == 0 && i + 1 < argc) {
            budget_path = argv[++i];
        } else if (strcmp(argv[i], "--bitstream-md5") == 0 && i + 1 < argc) {
            bitstream_md5 = argv[++i];
        } else if (strcmp(argv[i], "--commit") == 0 && i + 1 < argc) {
            commit = argv[++i];
        } else if (strcmp(argv[i], "--link") == 0 && i + 1 < argc) {
            link_name = argv[++i];
        } else if (strcmp(argv[i], "--fidelity") == 0 && i + 1 < argc) {
            fidelity_path = argv[++i];
        } else if (strcmp(argv[i], "--bridge-fidelity") == 0 && i + 1 < argc) {
            bridge_fidelity_path = argv[++i];
        } else if (strcmp(argv[i], "--allow-reused-bridge") == 0) {
            allow_reused = true;
        } else if (strcmp(argv[i], "--H") == 0 && i + 1 < argc) {
            const char *arg = argv[++i];
            char *end = NULL;
            const unsigned long v = strtoul(arg, &end, 10);
            if (arg[0] < '0' || arg[0] > '9' || *end != '\0' || v == 0UL || v > OTK_HARD_MAX_H) {
                fprintf(stderr, "otk-flywheel: --H takes 1 to %u steps, not '%s'\n", OTK_HARD_MAX_H, arg);
                return 2;
            }
            hard_h = (unsigned)v;
        } else if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            trace_path = argv[++i];
        } else if (strcmp(argv[i], "--criteria") == 0 && i + 1 < argc) {
            criteria_path = argv[++i];
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames_path = argv[++i];
        } else if (strcmp(argv[i], "--verdicts") == 0 && i + 1 < argc) {
            verdicts_path = argv[++i];
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            const char *arg = argv[++i];
            char *end = NULL;
            errno = 0;
            const unsigned long long v = strtoull(arg, &end, 10);
            if (arg[0] < '0' || arg[0] > '9' || *end != '\0' || errno != 0) {
                fprintf(stderr, "otk-flywheel: --seed takes a whole number from 0 to 2^64-1, not '%s'\n", arg);
                return 2;
            }
            link_seed = (uint64_t)v;
        } else if (strcmp(argv[i], "--driver") == 0 && i + 1 < argc) {
            if (!otk_driver_from_name(argv[++i], &kind)) {
                usage();
                return 2;
            }
        } else if (strcmp(argv[i], "--quiet") == 0) {
            quiet = 1;
        } else if (argv[i][0] != '-' && scenario_path == NULL) {
            scenario_path = argv[i];
        } else {
            usage();
            return 2;
        }
    }
    if (scenario_path == NULL || (verdicts_path != NULL && criteria_path == NULL)) {
        usage(); /* the verdict file is the oracle's: it needs --criteria */
        return 2;
    }
    const bool hard = kind == OTK_DRIVER_HARD;
    char budget_buf[1024];
    if (hard) {
        if (dev == NULL) {
            fprintf(stderr, "otk-flywheel: --driver hard needs --dev <tty>\n");
            return 2;
        }
        if (budget_path == NULL && trace_path == NULL) {
            fprintf(stderr, "otk-flywheel: a hard run writes its budget next to the trace: give --trace or --budget\n");
            return 2;
        }
        if (frames_path != NULL && can_if == NULL) {
            fprintf(stderr, "otk-flywheel: with --driver hard, --frames logs the CAN bus: it needs --can <if>\n");
            return 2;
        }
        if (budget_path == NULL) {
            budget_path_of(trace_path, budget_buf, sizeof(budget_buf));
            budget_path = budget_buf;
        }
    } else if (dev != NULL || can_if != NULL || budget_path != NULL || fidelity_path != NULL ||
               bridge_fidelity_path != NULL) {
        fprintf(stderr, "otk-flywheel: --dev, --can, --budget and --fidelity belong to --driver hard\n");
        return 2;
    }
    if ((fidelity_path == NULL) != (bridge_fidelity_path == NULL)) {
        fprintf(stderr, "otk-flywheel: --fidelity and --bridge-fidelity go together\n");
        return 2;
    }
    if (fidelity_path != NULL) {
        /* before the bridge is touched: a refused run never configures it */
        const int rc = check_fidelity(fidelity_path, bridge_fidelity_path, hard_h, declared_budget().rtt_p99_ns);
        if (rc != 0) {
            return rc;
        }
    }

    char err[600];
    if (!otk_scenario_load(scenario_path, &SIGNALS, &g_scenario, err, sizeof(err))) {
        fprintf(stderr, "otk-flywheel: %s\n", err);
        return 1;
    }
    if (!g_scenario.has_end) {
        fprintf(stderr, "otk-flywheel: %s has no `end` event\n", scenario_path);
        return 1;
    }

    if (criteria_path != NULL && !otk_oracle_load(&g_oracle, criteria_path, &ORACLE_TABLE, &g_scenario, &SIGNALS,
                                                  otk_ns_from_ms(STEP_MS), err, sizeof(err))) {
        fprintf(stderr, "otk-flywheel: %s: %s\n", criteria_path, err);
        return 2;
    }

    FILE *trace_fp = NULL;
    OtkTrace trace;
    if (trace_path != NULL) {
        trace_fp = fopen(trace_path, "w");
        if (trace_fp == NULL || !otk_trace_init(&trace, trace_fp, COLUMNS, sizeof(COLUMNS) / sizeof(COLUMNS[0]))) {
            perror("otk-flywheel: cannot write trace");
            return 1;
        }
    }

    if (frames_path != NULL && !hard) {
        /* In memory, both times are the simulation's: exact, so the error is 0. */
        const OtkFrameLogConfig cfg = { otk_ns_from_ms(STEP_MS), OTK_BUS_CLOCK_SIM, true, 0U, "otk-flywheel" };
        g_frames_fp = fopen(frames_path, "w");
        if (g_frames_fp == NULL || !otk_framelog_init(&g_frames, g_frames_fp, &cfg)) {
            perror("otk-flywheel: cannot write the frame log");
            return 1;
        }
    }

    Machine m;
    Dut d;
    memset(&m, 0, sizeof(m));
    memset(&d, 0, sizeof(d));
    OtkPlayer player;
    otk_player_init(&player, &g_scenario, machine_apply, &m);

    /* The in-memory bridge: frames from the machine reach the DUT in the same
     * step, through the speed link; frames from the DUT reach the machine at
     * the start of the next. */
    static OtkCanQueue to_dut;
    static OtkCanQueue to_machine;
    static OtkLink speed_link;
    otk_can_queue_init(&to_dut, 16U);
    otk_can_queue_init(&to_machine, 16U);
    otk_link_init(&speed_link, OTK_CAN_QUEUE_MAX, link_seed);
    static const OtkLinkFilter ONLY_SPEED = { true, false, ID_SPEED, OTK_CAN_SFF_MASK };
    for (int fault = 0; fault < (int)OTK_LINK_FAULT_COUNT; fault++) {
        otk_link_set_filter(&speed_link, (OtkLinkFault)fault, &ONLY_SPEED);
    }

    OtkDriver driver;
    otk_driver_init(&driver, kind, otk_ns_from_ms(STEP_MS));
    if (hard) {
        OtkHardLoopConfig hc;
        otk_hard_loop_defaults(&hc, STEP_TICKS);
        hc.h = hard_h;
        hc.need_tick_hz = TICK_HZ;
        hc.need_analog = 1U;
        hc.require_fresh = !allow_reused;
        if (otk_hard_loop_open(&g_hard, dev, &hc, err, sizeof(err)) != 0) {
            fprintf(stderr, "otk-flywheel: %s: %s\n", dev, err);
            return 1;
        }
        if (can_if != NULL) {
            if (!otk_socketcan_open(&g_can, can_if) || !otk_socketcan_enable_timestamps(&g_can, true)) {
                perror("otk-flywheel: cannot open the CAN interface with timestamps");
                otk_hard_loop_free(&g_hard);
                return 1;
            }
            g_can_on = true;
            otk_hard_loop_attach_can(&g_hard, &g_can, on_can_sent, NULL);
        }
        otk_driver_attach_hard(&driver, &g_hard);
        /* Bridge step k is simulated time k x 10 ms: step 0 holds the state at t = 0. */
        const OtkBridgeSetpoint sp0 = setpoint_of(0.0);
        otk_hard_loop_set(&g_hard, &sp0);
        (void)otk_hard_loop_next(&g_hard);
    }
    if (!quiet) {
        printf("otk-flywheel: %s, %s driver, until %.1f s\n", scenario_path, otk_driver_name(kind),
               otk_ns_to_s(g_scenario.end));
    }

    uint64_t dut_sent_ms = 0U; /* when the DUT sent what is in to_machine */
    while (!otk_player_done(&player, otk_ns_from_ms(m.t_ms)) && !otk_driver_failed(&driver)) {
        otk_player_advance(&player, otk_ns_from_ms(m.t_ms));
        for (size_t i = 0; i < otk_can_queue_count(&to_machine); i++) {
            /* sent by the DUT in the last step, at the same simulated time */
            log_frame(otk_ns_from_ms(m.t_ms), otk_ns_from_ms(dut_sent_ms), OTK_FRAME_FROM_DUT, NULL,
                      otk_can_queue_at(&to_machine, i), OTK_FRAME_OK);
            machine_rx(&m, otk_can_queue_at(&to_machine, i));
            if (criteria_path != NULL) {
                otk_oracle_frame(&g_oracle, otk_can_queue_at(&to_machine, i), otk_ns_from_ms(m.t_ms));
            }
        }
        otk_can_queue_clear(&to_machine);
        if (criteria_path != NULL) {
            otk_oracle_step(&g_oracle, otk_ns_from_ms(m.t_ms));
        }

        machine_step(&m, &to_dut);

        otk_link_set_faults(&speed_link, &m.speed_link);
        for (size_t i = 0; i < otk_can_queue_count(&to_dut); i++) {
            send_to_dut(&speed_link, otk_can_queue_at(&to_dut, i), otk_ns_from_ms(m.t_ms));
        }
        otk_can_queue_clear(&to_dut);
        OtkCanFrame rx;
        while (deliver_to_dut(&speed_link, otk_ns_from_ms(m.t_ms), &rx)) {
            dut_rx(&d, &rx, m.t_ms);
        }
        dut_step(&d, m.t_ms, &to_machine);
        dut_sent_ms = m.t_ms;

        if (trace_fp != NULL) {
            trace_row(&trace, &m, &d);
        }
        if (!quiet && m.t_ms % 500U == 0U) {
            printf("[%5.2fs] target %6.1f rpm | speed %7.1f rpm | duty %5.1f %% | load %5.2f Nm | DUT %s\n",
                   (double)m.t_ms / 1000.0, m.target_rpm, rpm_of(&m), 100.0 * m.duty, m.load_nm,
                   DUT_STATE_NAMES[d.state]);
        }
        if (hard) {
            /* the bridge's step t / 10 ms: the crank turns at the flywheel's speed */
            const uint32_t step = (uint32_t)(m.t_ms / STEP_MS);
            const OtkBridgeSetpoint sp = setpoint_of(rpm_of(&m));
            otk_hard_loop_set(&g_hard, &sp);
            if (g_can_on && !m.speed_lost) {
                const OtkCanFrame f = u16_frame(ID_SPEED, rpm_of(&m));
                (void)otk_hard_loop_can_at(&g_hard, &f, step);
            }
            hard_read_bus(step);
        }
        otk_driver_wait(&driver);
    }

    int hard_rc = 0;
    if (hard) {
        const OtkHardResult result = otk_hard_loop_stop(&g_hard);
        otk_hard_sleep_until(otk_hard_now_ns() + 50000000U); /* the last frames' echoes */
        hard_read_bus((uint32_t)(m.t_ms / STEP_MS));
        if (g_can_on) {
            otk_socketcan_close(&g_can);
        }
        char usb[256];
        (void)otk_hard_usb_path(dev, usb, sizeof(usb));
        const OtkHardBudget budget = declared_budget();
        const OtkHardRunMeta meta = { link_name, usb, bitstream_md5, commit, "otk-flywheel" };
        const OtkHardCanErr ce = can_err();
        FILE *bf = fopen(budget_path, "w");
        const bool written = bf != NULL && otk_hard_budget_write(bf, &g_hard, result, &budget, &meta, &ce);
        if (bf == NULL || fclose(bf) != 0 || !written) {
            fprintf(stderr, "otk-flywheel: cannot write the budget %s\n", budget_path);
            hard_rc = 1;
        }
        if (frames_path != NULL && !write_hard_frames(frames_path)) {
            hard_rc = 1;
        }
        const OtkHardStats *st = &g_hard.stats;
        printf("otk-flywheel: hard run %s: %u bridge steps, %u underruns, %u late, %u timeouts; round trip p50 %.0f us, "
               "p99 %.0f us; mapping bound %.1f us; REALTIME -> MONOTONIC %llu ns (%llu samples dropped); budget in %s\n",
               otk_hard_result_name(result), st->steps, st->underruns, st->late, st->timeouts,
               (double)otk_hard_loop_rtt_pct(&g_hard, 50.0) / 1e3, (double)otk_hard_loop_rtt_pct(&g_hard, 99.0) / 1e3,
               st->bound_max_ns / 1e3, (unsigned long long)otk_rt_mono_err_ns(&g_hard.rt),
               (unsigned long long)g_hard.rt.rejected, budget_path);
        if (g_hard_oom) {
            fprintf(stderr, "otk-flywheel: out of memory: the frame log or the CAN figures are incomplete\n");
            hard_rc = 1;
        }
        if (hard_rc == 0) {
            hard_rc = result == OTK_HARD_OK ? 0 : result == OTK_HARD_DEGRADED ? EXIT_DEGRADED : EXIT_ABORTED;
        }
        otk_hard_loop_free(&g_hard);
        free(g_rows);
        free(g_tx_err);
    }

    printf("otk-flywheel: done at %.2f s, %llu steps, %.1f rpm, DUT %s", (double)m.t_ms / 1000.0,
           (unsigned long long)driver.steps, rpm_of(&m), DUT_STATE_NAMES[d.state]);
    if (kind == OTK_DRIVER_PACED) {
        printf(", %u late steps, latest wake-up %.3f ms late", (unsigned)driver.pacer.overruns,
               (double)driver.pacer.max_late / 1e6);
    }
    printf("\n");

    if (trace_fp != NULL) {
        const int trace_ok = otk_trace_ok(&trace);
        if (!trace_ok) {
            fprintf(stderr, "otk-flywheel: trace: %s\n", otk_trace_error(&trace));
        }
        if (fclose(trace_fp) != 0 || !trace_ok) {
            return 1;
        }
    }
    if (g_frames_fp != NULL) {
        const int frames_ok = otk_framelog_ok(&g_frames);
        if (!frames_ok) {
            fprintf(stderr, "otk-flywheel: frame log: %s\n", otk_framelog_error(&g_frames));
        }
        if (fclose(g_frames_fp) != 0 || !frames_ok) {
            return 1;
        }
    }
    if (to_dut.overflows != 0U || to_machine.overflows != 0U || speed_link.overflows != 0U) {
        return 1;
    }
    if (criteria_path != NULL) {
        /* The step at `end` is graded too, with the frames the DUT sent in the last step. */
        for (size_t i = 0; i < otk_can_queue_count(&to_machine); i++) {
            otk_oracle_frame(&g_oracle, otk_can_queue_at(&to_machine, i), otk_ns_from_ms(m.t_ms));
        }
        otk_oracle_step(&g_oracle, otk_ns_from_ms(m.t_ms));
        const bool pass = otk_oracle_report(&g_oracle, stdout);
        const size_t total = otk_oracle_count(&g_oracle);
        if (verdicts_path != NULL) {
            FILE *vf = fopen(verdicts_path, "w");
            const bool written = vf != NULL && otk_oracle_write_verdicts(&g_oracle, vf);
            if (vf == NULL || fclose(vf) != 0 || !written) {
                fprintf(stderr, "otk-flywheel: cannot write %s\n", verdicts_path);
                return 1;
            }
        }
        if (!pass) {
            printf("otk-flywheel: FAIL (%zu of %zu criteria failed)\n", otk_oracle_failures(&g_oracle), total);
            return 1;
        }
        printf("otk-flywheel: PASS (%zu of %zu criteria)\n", total, total);
    }
    return hard_rc;
}
