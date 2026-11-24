/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file flywheel.c
 * @brief otk-flywheel: the smallest machine that uses all of open-tick v0.1.
 *
 *     otk-flywheel examples/flywheel/step.csv [--driver free-run|paced] [--trace out.csv] [--quiet]
 *                  [--criteria examples/flywheel/step.accept.csv [--verdicts out.verdicts.csv]]
 *
 * A DC motor spins a flywheel; a toy speed controller (the "device under
 * test") holds the speed the operator asks for. They talk CAN through
 * open-tick's in-memory bridge, exactly as they would over a bus:
 *
 *     machine -> DUT   0x100 SPEED    every 10 ms   uint16 LE, rpm
 *     machine -> DUT   0x101 TARGET   every 20 ms   uint16 LE, rpm (operator panel)
 *     DUT -> machine   0x200 DUTY     every 10 ms   uint8, 0.5 %/bit
 *
 * This is an example, not an application: the plant is a first-order sketch
 * and the message set is made up. It exists to show the pieces together:
 * a signal table, a scenario, the player, CAN queues, a trace and a driver.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <opentick/can.h>
#include <opentick/host/driver.h>
#include <opentick/oracle.h>
#include <opentick/player.h>
#include <opentick/scenario.h>
#include <opentick/trace.h>

#define STEP_MS 10U
#define ID_SPEED 0x100U
#define ID_TARGET 0x101U
#define ID_DUTY 0x200U
#define PI 3.14159265358979323846

/* ---- Signals the scenario can set ---------------------------------------- */

enum { SIG_TARGET_RPM, SIG_LOAD_NM, SIG_FAULT_SPEED_LOST, SIG_COUNT };
static const char *const SIGNAL_NAMES[SIG_COUNT] = {
    [SIG_TARGET_RPM] = "target_rpm",
    [SIG_LOAD_NM] = "load_nm",
    [SIG_FAULT_SPEED_LOST] = "fault.speed_lost",
};
static const OtkSignalTable SIGNALS = { SIGNAL_NAMES, SIG_COUNT };

/* ---- The machine: flywheel, motor, operator panel ------------------------ */

typedef struct {
    /* signals */
    double target_rpm;
    double load_nm;
    int speed_lost;
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
    fprintf(stderr, "usage: otk-flywheel <scenario.csv> [--driver free-run|paced] [--trace <out.csv>] [--quiet]\n"
                    "                   [--criteria <criteria.csv> [--verdicts <out.csv>]]\n");
}

int main(int argc, char **argv) {
    const char *scenario_path = NULL;
    const char *trace_path = NULL;
    const char *criteria_path = NULL;
    const char *verdicts_path = NULL;
    OtkDriverKind kind = OTK_DRIVER_FREE_RUN;
    int quiet = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            trace_path = argv[++i];
        } else if (strcmp(argv[i], "--criteria") == 0 && i + 1 < argc) {
            criteria_path = argv[++i];
        } else if (strcmp(argv[i], "--verdicts") == 0 && i + 1 < argc) {
            verdicts_path = argv[++i];
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

    Machine m;
    Dut d;
    memset(&m, 0, sizeof(m));
    memset(&d, 0, sizeof(d));
    OtkPlayer player;
    otk_player_init(&player, &g_scenario, machine_apply, &m);

    /* The in-memory bridge: frames from the machine reach the DUT in the same
     * step; frames from the DUT reach the machine at the start of the next. */
    static OtkCanQueue to_dut;
    static OtkCanQueue to_machine;
    otk_can_queue_init(&to_dut, 16U);
    otk_can_queue_init(&to_machine, 16U);

    OtkDriver driver;
    otk_driver_init(&driver, kind, otk_ns_from_ms(STEP_MS));
    if (!quiet) {
        printf("otk-flywheel: %s, %s driver, until %.1f s\n", scenario_path, otk_driver_name(kind),
               otk_ns_to_s(g_scenario.end));
    }

    while (!otk_player_done(&player, otk_ns_from_ms(m.t_ms))) {
        otk_player_advance(&player, otk_ns_from_ms(m.t_ms));
        for (size_t i = 0; i < otk_can_queue_count(&to_machine); i++) {
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

        for (size_t i = 0; i < otk_can_queue_count(&to_dut); i++) {
            dut_rx(&d, otk_can_queue_at(&to_dut, i), m.t_ms);
        }
        otk_can_queue_clear(&to_dut);
        dut_step(&d, m.t_ms, &to_machine);

        if (trace_fp != NULL) {
            trace_row(&trace, &m, &d);
        }
        if (!quiet && m.t_ms % 500U == 0U) {
            printf("[%5.2fs] target %6.1f rpm | speed %7.1f rpm | duty %5.1f %% | load %5.2f Nm | DUT %s\n",
                   (double)m.t_ms / 1000.0, m.target_rpm, rpm_of(&m), 100.0 * m.duty, m.load_nm,
                   DUT_STATE_NAMES[d.state]);
        }
        otk_driver_wait(&driver);
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
    if (to_dut.overflows != 0U || to_machine.overflows != 0U) {
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
    return 0;
}
