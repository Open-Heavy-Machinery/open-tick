/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file hard_probe.c
 * @brief otk-hard-probe: measures a signal-level bridge's link with the hard
 *        driver's code (opentick/host/hard.h), in C.
 *
 *   otk-hard-probe --dev <tty> [--baud 3000000] [--seconds S] [--csv FILE] MODE [options]
 *
 * Modes:
 *   cycle                    back-to-back exchanges: round trip, and the
 *                            link's forwarding cycle from the arrival ticks
 *   sweep --walk-ns W        one exchange every two cycles + W, so the send time
 *                            walks through the cycle: delay by send phase
 *   locked --lead-ns L       learn the cycle, then send every exchange so that
 *                            its request and its reply are on the wire and
 *                            L ns of margin are left before a forwarding
 *                            instant: delay, sync quality
 *   hold --H N [--lead-ns L] a real run: 1 ms steps, keep the buffer near N
 *                            setpoints; underruns and the lowest fill. With
 *                            --lead-ns the exchanges are locked, otherwise
 *                            back to back.
 *
 * Every mode prints a summary; --csv writes every exchange
 * (t1_ns,t4_ns,t2_tick,t3_tick,late_ns,step,fill,underruns,error); late_ns is how
 * late a scheduled request went out against its planned time. --spin-us S
 * (default 300) is how long before a planned send the probe stops sleeping
 * and spins; 0 sleeps all the way.
 */

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opentick/bridge.h>
#include <opentick/host/hard.h>

#define TICK_HZ 27000000u
#define STEP_TICKS 27000u        /* 1 ms */
#define P_GUESS_NS 6000000.0     /* 6 ms: the BL616's bridge -> host cycle, roughly (F-14) */
#define TIMEOUT_NS 200000000u    /* 200 ms */
#define MAX_SAMPLES 200000
#define BINS 24                  /* the sweep's phase bins */

typedef struct {
    uint64_t t1, t4, t2, t3;
    int64_t late;  /**< t1 minus the planned send time, ns (0 when not scheduled) */
    uint32_t step;
    uint16_t fill;
    uint32_t underruns;
    uint16_t error;
} Row;

static Row rows[MAX_SAMPLES];
static uint32_t baud_ = 3000000u;
static uint64_t planned_; /* the planned send time of the next exchange, 0 when not scheduled */
static uint64_t spin_ns_ = OTK_HARD_DEFAULT_SPIN_NS;

/* Waits for a planned send time; the exchange that follows logs how late it went out. */
static void wait_send(uint64_t at) {
    (void)otk_hard_wait_until(at, spin_ns_);
    planned_ = at;
}

/* Time on the wire of `bytes` at 8N1. */
static double wire_ns(size_t bytes) {
    return (double)bytes * 10.0 * 1e9 / (double)baud_;
}

/* Bytes of an exchange's request with n setpoints, and of a reply with no capture. */
static size_t request_bytes(unsigned n) {
    return OTK_BRIDGE_FRAMING_BYTES + 5u + OTK_BRIDGE_SETPOINT_BYTES * n;
}
#define REPLY_BYTES (OTK_BRIDGE_FRAMING_BYTES + OTK_BRIDGE_REPLY_FIXED_BYTES)
static size_t n_rows;
static OtkHardLink link_;
static FILE *csv;

static int cmp_u64(const void *a, const void *b) {
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* Nearest-rank percentile; sorts v. */
static double pct(uint64_t *v, size_t n, double p) {
    if (n == 0) {
        return 0;
    }
    qsort(v, n, sizeof(*v), cmp_u64);
    size_t i = (size_t)ceil(p / 100.0 * (double)n);
    return (double)v[(i == 0 ? 1 : i) - 1];
}

/* One exchange; its reply decoded into a row. */
static int exchange(const OtkBridgeRequest *q, OtkBridgeReply *d) {
    uint8_t p[OTK_BRIDGE_MAX_PAYLOAD];
    const size_t len = otk_bridge_encode_request(q, p, sizeof(p));
    OtkHardReply r;
    const int rc = otk_hard_transact(&link_, OTK_BRIDGE_EXCHANGE, p, len, false, TIMEOUT_NS, &r);
    if (rc != 0) {
        return rc;
    }
    if (r.type != (OTK_BRIDGE_EXCHANGE | OTK_BRIDGE_REPLY) || !otk_bridge_decode_reply(r.payload, r.len, d)) {
        return -1;
    }
    if (n_rows < MAX_SAMPLES) {
        rows[n_rows++] = (Row){ r.t1, r.t4, d->t2, d->t3, planned_ ? (int64_t)(r.t1 - planned_) : 0, d->step, d->fill,
                                d->underruns, d->error };
    }
    if (csv) {
        fprintf(csv, "%llu,%llu,%llu,%llu,%lld,%u,%u,%u,%u\n", (unsigned long long)r.t1, (unsigned long long)r.t4,
                (unsigned long long)d->t2, (unsigned long long)d->t3, planned_ ? (long long)(r.t1 - planned_) : 0LL,
                d->step, d->fill, d->underruns, d->error);
    }
    planned_ = 0;
    return 0;
}

static int control(uint8_t type, const uint8_t *p, size_t len, OtkHardReply *r) {
    const int rc = otk_hard_transact(&link_, type, p, len, false, TIMEOUT_NS, r);
    if (rc != 0) {
        fprintf(stderr, "otk-hard-probe: no reply to message 0x%02x (%d)\n", type, rc);
    }
    return rc;
}

static int configure(void) {
    const OtkBridgeConfig c = { STEP_TICKS, 60u, 2u, 0u, 0u, 0x80000000u, 0x00u, 0x3Fu, 100u };
    uint8_t p[32];
    const size_t len = otk_bridge_encode_configure(&c, p, sizeof(p));
    OtkHardReply r;
    OtkBridgeConfigReply d;
    if (control(OTK_BRIDGE_CONFIGURE, p, len, &r) != 0 || !otk_bridge_decode_configure_reply(r.payload, r.len, &d) ||
        d.status != OTK_BRIDGE_STATUS_OK) {
        fprintf(stderr, "otk-hard-probe: configure failed\n");
        return -1;
    }
    printf("bridge: tick %u Hz, %u captures, %u analog outputs of %u bits\n", d.tick_hz, d.n_capture, d.n_analog,
           d.analog_bits);
    return 0;
}

/* ---------------------------------------------------------------- analysis over all rows */

/* Round trips, and delay = round trip minus the time inside the bridge, in ns. */
static void print_rtt(const char *what) {
    uint64_t *v = malloc(n_rows * sizeof(uint64_t)), *dl = malloc(n_rows * sizeof(uint64_t));
    size_t below300 = 0, below500 = 0, below1000 = 0;
    for (size_t i = 0; i < n_rows; i++) {
        v[i] = rows[i].t4 - rows[i].t1;
        const double inside = (double)(rows[i].t3 - rows[i].t2) * 1e9 / TICK_HZ;
        const double d = (double)v[i] - inside;
        dl[i] = d > 0 ? (uint64_t)d : 0;
        below300 += dl[i] < 300000u;
        below500 += dl[i] < 500000u;
        below1000 += dl[i] < 1000000u;
    }
    printf("%s: %zu exchanges, %u timeouts\n", what, n_rows, link_.timeouts);
    {
        uint64_t *late = malloc(n_rows * sizeof(uint64_t));
        size_t nl = 0;
        for (size_t i = 0; i < n_rows; i++) {
            if (rows[i].late != 0) {
                late[nl++] = rows[i].late > 0 ? (uint64_t)rows[i].late : 0u;
            }
        }
        if (nl > 0) {
            printf("  sends late against their plan us: p50 %.1f p99 %.1f max %.1f (spin %llu us)\n", pct(late, nl, 50) / 1e3,
                   pct(late, nl, 99) / 1e3, pct(late, nl, 100) / 1e3, (unsigned long long)(spin_ns_ / 1000u));
        }
        free(late);
    }
    printf("  round trip us: p1 %.0f p50 %.0f p99 %.0f max %.0f\n", pct(v, n_rows, 1) / 1e3, pct(v, n_rows, 50) / 1e3,
           pct(v, n_rows, 99) / 1e3, pct(v, n_rows, 100) / 1e3);
    printf("  delay (round trip minus the bridge's own time): below 300 us %.1f %%, below 500 us %.1f %%, below 1 ms %.1f %%\n",
           100.0 * (double)below300 / (double)n_rows, 100.0 * (double)below500 / (double)n_rows,
           100.0 * (double)below1000 / (double)n_rows);
    free(v);
    free(dl);
}

/* A line through the lowest-delay samples of all rows (delay below `thr` ns): drift and residuals. */
static void print_sync(double thr_ns) {
    double st = 0, so = 0;
    size_t n = 0;
    const uint64_t t_ref = rows[0].t1;
    double *t = malloc(n_rows * sizeof(double)), *o = malloc(n_rows * sizeof(double));
    uint64_t last = 0, gap = 0;
    for (size_t i = 0; i < n_rows; i++) {
        const double t1 = (double)(rows[i].t1 - t_ref), t4 = (double)(rows[i].t4 - t_ref);
        const double t2 = (double)rows[i].t2 * 1e9 / TICK_HZ, t3 = (double)rows[i].t3 * 1e9 / TICK_HZ;
        const double delay = (t4 - t1) - (t3 - t2);
        if (delay < thr_ns) {
            t[n] = (t1 + t4) / 2;
            o[n] = ((t2 - t1) + (t3 - t4)) / 2;
            st += t[n];
            so += o[n];
            if (last != 0 && rows[i].t1 - last > gap) {
                gap = rows[i].t1 - last;
            }
            last = rows[i].t1;
            n++;
        }
    }
    if (n < 3) {
        printf("  sync, delay < %.0f us: %zu samples, too few\n", thr_ns / 1e3, n);
        free(t);
        free(o);
        return;
    }
    const double mt = st / (double)n, mo = so / (double)n;
    double stt = 0, sto = 0;
    for (size_t i = 0; i < n; i++) {
        stt += (t[i] - mt) * (t[i] - mt);
        sto += (t[i] - mt) * (o[i] - mo);
    }
    const double b = sto / stt;
    uint64_t *res = malloc(n * sizeof(uint64_t));
    for (size_t i = 0; i < n; i++) {
        res[i] = (uint64_t)fabs(o[i] - (mo + b * (t[i] - mt)));
    }
    printf("  sync, delay < %.0f us: %zu samples (%.1f %%), longest gap %.2f s, drift %+.2f ppm, |offset - line| us p50 %.0f p99 %.0f max %.0f\n",
           thr_ns / 1e3, n, 100.0 * (double)n / (double)n_rows, (double)gap / 1e9, b * 1e6, pct(res, n, 50) / 1e3, pct(res, n, 99) / 1e3,
           pct(res, n, 100) / 1e3);
    free(t);
    free(o);
    free(res);
}

/* The bridge -> host forwarding cycle, in host ns, from the replies' arrival times (T4). */
static bool fit_cycle(size_t from, size_t to, double *period, double *t0, double *rms) {
    uint64_t *t4 = malloc((to - from) * sizeof(uint64_t));
    for (size_t i = from; i < to; i++) {
        t4[i - from] = rows[i].t4;
    }
    const bool ok = otk_hard_cycle_fit(t4, to - from, P_GUESS_NS, period, t0, rms);
    free(t4);
    return ok;
}

/* ---------------------------------------------------------------- modes */

static void back_to_back(uint64_t until) {
    OtkBridgeRequest q;
    memset(&q, 0, sizeof(q));
    OtkBridgeReply d;
    while (otk_hard_now_ns() < until) {
        if (exchange(&q, &d) < 0) {
            break;
        }
    }
}

static int mode_cycle(double seconds) {
    back_to_back(otk_hard_now_ns() + (uint64_t)(seconds * 1e9));
    print_rtt("cycle (back to back)");
    double p, t0, rms;
    if (fit_cycle(0, n_rows, &p, &t0, &rms)) {
        printf("  bridge -> host forwarding cycle from the replies' arrival: period %.3f us, rms of the arrivals around it %.1f us\n",
               p / 1e3, rms / 1e3);
    }
    print_sync(300000);
    print_sync(500000);
    return 0;
}

static int mode_sweep(double seconds, double walk_ns) {
    /* learn the cycle first, back to back, for one second */
    back_to_back(otk_hard_now_ns() + 1000000000u);
    double p, t0, rms;
    if (!fit_cycle(0, n_rows, &p, &t0, &rms)) {
        fprintf(stderr, "otk-hard-probe: no cycle found\n");
        return 1;
    }
    const size_t first = n_rows;
    /* every second cycle, plus the walk: the previous reply (up to one cycle) is back before the next send */
    const double step_ns = 2.0 * p + walk_ns;
    const uint64_t start = otk_hard_now_ns() + 10000000u, until = start + (uint64_t)(seconds * 1e9);
    OtkBridgeRequest q;
    memset(&q, 0, sizeof(q));
    OtkBridgeReply d;
    for (uint64_t k = 0;; k++) {
        const uint64_t at = start + (uint64_t)((double)k * step_ns);
        if (at >= until) {
            break;
        }
        wait_send(at);
        if (exchange(&q, &d) < 0) {
            break;
        }
    }
    /* The forwarding instants, from when the replies arrived. */
    if (!fit_cycle(first, n_rows, &p, &t0, &rms)) {
        fprintf(stderr, "otk-hard-probe: no cycle in the sweep\n");
        return 1;
    }
    printf("sweep: bridge -> host forwarding cycle %.3f us, replies within %.1f us rms of it\n", p / 1e3, rms / 1e3);
    uint64_t *by[BINS];
    size_t nb[BINS] = { 0 };
    for (int b = 0; b < BINS; b++) {
        by[b] = malloc(n_rows * sizeof(uint64_t));
    }
    for (size_t i = first; i < n_rows; i++) {
        /* how long before the next forwarding instant the request was written */
        double ph = fmod((double)rows[i].t1 - t0, p);
        if (ph < 0) {
            ph += p;
        }
        const double lead = p - ph;
        int b = (int)(lead / p * BINS);
        if (b >= BINS) {
            b = BINS - 1;
        }
        by[b][nb[b]++] = rows[i].t4 - rows[i].t1;
    }
    printf("  delay by how long before a forwarding instant the request was written:\n");
    printf("  lead us        n   round trip p10 us   p50 us   p90 us\n");
    for (int b = 0; b < BINS; b++) {
        const double lo = p * b / BINS / 1e3, hi = p * (b + 1) / BINS / 1e3;
        const size_t n = nb[b];
        printf("  %5.0f-%5.0f %5zu   %10.0f %8.0f %8.0f\n", lo, hi, n, pct(by[b], n, 10) / 1e3, pct(by[b], n, 50) / 1e3,
               pct(by[b], n, 90) / 1e3);
        free(by[b]);
    }
    return 0;
}

/* Locked scheduling state: the cycle, refitted from recent arrivals. */
typedef struct {
    double period, t0;
    size_t from;
} Cycle;

/* The next send time, `lead` ns before a forwarding instant, not before `not_before`. */
static uint64_t next_locked_send(const Cycle *c, double lead, uint64_t not_before) {
    double k = ceil(((double)not_before + lead - c->t0) / c->period);
    return (uint64_t)(c->t0 + k * c->period - lead);
}

static void refit_cycle(Cycle *c) {
    const size_t window = 400;
    const size_t from = n_rows > window ? n_rows - window : 0;
    double p, t0, rms;
    if (n_rows - from >= 16 && fit_cycle(from, n_rows, &p, &t0, &rms)) {
        c->period = p;
        c->t0 = t0;
    }
}

static int learn(OtkHardSync *s, Cycle *c) {
    otk_hard_sync_init(s, TICK_HZ, 100000000u);
    const size_t first = n_rows;
    back_to_back(otk_hard_now_ns() + 2000000000u);
    for (size_t i = first; i < n_rows; i++) {
        const OtkHardSample x = { rows[i].t1, rows[i].t4, rows[i].t2, rows[i].t3 };
        otk_hard_sync_add(s, &x);
    }
    double rms;
    if (!fit_cycle(first, n_rows, &c->period, &c->t0, &rms)) {
        fprintf(stderr, "otk-hard-probe: no cycle found\n");
        return -1;
    }
    return 0;
}

static int mode_locked(double seconds, double lead_ns) {
    OtkHardSync s;
    Cycle c;
    if (learn(&s, &c) != 0) {
        return 1;
    }
    const size_t first = n_rows;
    const uint64_t until = otk_hard_now_ns() + (uint64_t)(seconds * 1e9);
    OtkBridgeRequest q;
    memset(&q, 0, sizeof(q));
    OtkBridgeReply d;
    while (otk_hard_now_ns() < until) {
        const uint64_t at = next_locked_send(&c, lead_ns + wire_ns(request_bytes(0) + REPLY_BYTES), otk_hard_now_ns() + 200000u);
        wait_send(at);
        if (exchange(&q, &d) < 0) {
            break;
        }
        const OtkHardSample x = { rows[n_rows - 1].t1, rows[n_rows - 1].t4, rows[n_rows - 1].t2, rows[n_rows - 1].t3 };
        otk_hard_sync_add(&s, &x);
        refit_cycle(&c);
    }
    /* report on the locked part only */
    memmove(rows, &rows[first], (n_rows - first) * sizeof(Row));
    n_rows -= first;
    printf("locked, %.0f us of margin plus %.0f us on the wire before each forwarding instant (cycle %.3f us):\n",
           lead_ns / 1e3, wire_ns(request_bytes(0) + REPLY_BYTES) / 1e3, c.period / 1e3);
    print_rtt("locked");
    print_sync(300000);
    print_sync(500000);
    printf("  running mapping at the end: bound %.1f us over its last %u bins\n", otk_hard_sync_bound_ns(&s) / 1e3, s.used);
    return 0;
}

static OtkBridgeSetpoint setpoint(uint32_t k) {
    OtkBridgeSetpoint sp;
    uint32_t inc = 0;
    (void)otk_bridge_phase_inc(1500.0, TICK_HZ, &inc);
    sp.crank_phase_inc = inc;
    sp.analog[0] = (uint16_t)(k & 0xFFFFu); /* the step, so the applied setpoint can be checked */
    sp.analog[1] = otk_bridge_analog(0.25);
    sp.analog[2] = otk_bridge_analog(0.5);
    sp.analog[3] = otk_bridge_analog(0.75);
    return sp;
}

static int mode_hold(double seconds, unsigned h, double lead_ns) {
    if (h == 0 || h > OTK_BRIDGE_MAX_SETPOINTS) {
        fprintf(stderr, "otk-hard-probe: --H must be 1..36\n");
        return 2;
    }
    OtkHardSync s;
    Cycle c;
    const bool locked = lead_ns >= 0;
    if (locked && learn(&s, &c) != 0) {
        return 1;
    }
    n_rows = 0;
    /* fill, start */
    OtkBridgeRequest q;
    memset(&q, 0, sizeof(q));
    q.first_step = 0;
    q.n = (uint8_t)h;
    for (unsigned i = 0; i < h; i++) {
        q.sp[i] = setpoint(i);
    }
    OtkBridgeReply d;
    if (exchange(&q, &d) != 0 || d.error != 0) {
        fprintf(stderr, "otk-hard-probe: the first fill failed\n");
        return 1;
    }
    uint32_t next_q = h;
    OtkHardReply r;
    OtkBridgeStartReply sr;
    if (control(OTK_BRIDGE_START, NULL, 0, &r) != 0 || !otk_bridge_decode_start_reply(r.payload, r.len, &sr) || sr.status != 0) {
        fprintf(stderr, "otk-hard-probe: start failed\n");
        return 1;
    }
    /* The bridge's tick now, from the last request: on the BL616 the request
     * reaches the bridge almost at once (T2 ~ T1), while the reply may wait up
     * to a cycle. The symmetric mapping (§4) would put the bridge up to half
     * that wait behind, and every setpoint aimed with it would be late. */
    uint64_t last_t1 = r.t1, last_t2 = 0;
    last_t1 = rows[n_rows - 1].t1; /* the fill's exchange, the last one decoded */
    last_t2 = rows[n_rows - 1].t2;
    const uint64_t t_start = sr.t_start;
    const uint64_t until = otk_hard_now_ns() + (uint64_t)(seconds * 1e9);
    uint16_t min_fill = 0xFFFF;
    uint32_t applied_wrong = 0, checked = 0;
    while (otk_hard_now_ns() < until) {
        uint32_t n = 0;
        if (locked) {
            /* aim for the steps up to H after the send; the request's size sets how early to send */
            const double guess_n = 7.0; /* a cycle of 6 ms consumes about 6 setpoints */
            const uint64_t at = next_locked_send(&c, lead_ns + wire_ns(request_bytes((unsigned)guess_n) + REPLY_BYTES),
                                                 otk_hard_now_ns() + 200000u);
            wait_send(at);
        }
        const uint64_t now = otk_hard_now_ns();
        const double tick_now = (double)last_t2 + (double)(now - last_t1) * (TICK_HZ / 1e9);
        const uint32_t step_now = tick_now > (double)t_start ? (uint32_t)((tick_now - (double)t_start) / STEP_TICKS) : 0u;
        const uint32_t target = step_now + h + 1u;
        n = target > next_q ? target - next_q : 0u;
        if (n > OTK_BRIDGE_MAX_SETPOINTS) {
            n = OTK_BRIDGE_MAX_SETPOINTS;
        }
        q.first_step = next_q;
        q.n = (uint8_t)n;
        for (uint32_t i = 0; i < n; i++) {
            q.sp[i] = setpoint(next_q + i);
        }
        if (exchange(&q, &d) != 0) {
            fprintf(stderr, "otk-hard-probe: exchange failed at step about %u\n", step_now);
            break;
        }
        if (!(d.error & OTK_BRIDGE_ERR_SEQUENCE)) {
            next_q += n;
        }
        last_t1 = rows[n_rows - 1].t1;
        last_t2 = rows[n_rows - 1].t2;
        if (d.state == OTK_BRIDGE_RUNNING && d.fill < min_fill) {
            min_fill = d.fill;
        }
        if (d.state == OTK_BRIDGE_RUNNING) {
            checked++;
            const OtkBridgeSetpoint want = setpoint(d.step);
            if (d.underruns == 0 && (d.applied.crank_phase_inc != want.crank_phase_inc || d.applied.analog[0] != want.analog[0])) {
                applied_wrong++;
            }
        }
        if (locked) {
            const OtkHardSample x = { rows[n_rows - 1].t1, rows[n_rows - 1].t4, rows[n_rows - 1].t2, rows[n_rows - 1].t3 };
            otk_hard_sync_add(&s, &x);
            refit_cycle(&c);
        }
    }
    OtkBridgeStopReply st;
    if (control(OTK_BRIDGE_STOP, NULL, 0, &r) != 0 || !otk_bridge_decode_stop_reply(r.payload, r.len, &st)) {
        fprintf(stderr, "otk-hard-probe: stop failed\n");
        return 1;
    }
    printf("hold H = %u steps (1 ms), %s: %u steps, underruns %u (%.3f %%), lowest fill at a reply %u, "
           "applied setpoint wrong %u of %u checked\n",
           h, locked ? "locked" : "back to back", st.step, st.underruns, 100.0 * st.underruns / (st.step ? st.step : 1u),
           min_fill, applied_wrong, checked);
    if (locked) {
        printf("  locked: %.0f us of margin plus the request's and reply's time on the wire before each forwarding instant\n",
               lead_ns / 1e3);
    }
    print_rtt("hold");
    return st.underruns == 0 ? 0 : 3;
}

int main(int argc, char **argv) {
    const char *dev = NULL, *csv_path = NULL, *mode = NULL;
    uint32_t baud = 3000000u;
    double seconds = 10, walk_ns = 37000, lead_ns = -1;
    unsigned h = OTK_HARD_DEFAULT_H;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!strcmp(a, "--dev") && v) { dev = v; i++; }
        else if (!strcmp(a, "--baud") && v) { baud = (uint32_t)strtoul(v, NULL, 10); baud_ = baud; i++; }
        else if (!strcmp(a, "--seconds") && v) { seconds = atof(v); i++; }
        else if (!strcmp(a, "--csv") && v) { csv_path = v; i++; }
        else if (!strcmp(a, "--walk-ns") && v) { walk_ns = atof(v); i++; }
        else if (!strcmp(a, "--lead-ns") && v) { lead_ns = atof(v); i++; }
        else if (!strcmp(a, "--H") && v) { h = (unsigned)strtoul(v, NULL, 10); i++; }
        else if (!strcmp(a, "--spin-us") && v) { spin_ns_ = (uint64_t)strtoull(v, NULL, 10) * 1000u; i++; }
        else if (a[0] != '-' && !mode) { mode = a; }
        else {
            fprintf(stderr, "otk-hard-probe: unknown argument %s\n", a);
            return 2;
        }
    }
    if (!dev || !mode) {
        fprintf(stderr, "usage: otk-hard-probe --dev <tty> [--baud B] [--seconds S] [--csv F] "
                        "cycle | sweep [--walk-ns W] | locked --lead-ns L | hold --H N [--lead-ns L]\n");
        return 2;
    }
    if (otk_hard_open(&link_, dev, baud) != 0) {
        perror(dev);
        return 2;
    }
    if (csv_path) {
        csv = fopen(csv_path, "w");
        if (!csv) {
            perror(csv_path);
            return 2;
        }
        fprintf(csv, "t1_ns,t4_ns,t2_tick,t3_tick,late_ns,step,fill,underruns,error\n");
    }
    const size_t banner = otk_hard_drain(&link_, 300000000u, 3000000000u);
    printf("drained %zu bytes before the first exchange\n", banner);
    if (configure() != 0) {
        return 1;
    }
    int rc;
    if (!strcmp(mode, "cycle")) {
        rc = mode_cycle(seconds);
    } else if (!strcmp(mode, "sweep")) {
        rc = mode_sweep(seconds, walk_ns);
    } else if (!strcmp(mode, "locked")) {
        rc = lead_ns < 0 ? 2 : mode_locked(seconds, lead_ns);
    } else if (!strcmp(mode, "hold")) {
        rc = mode_hold(seconds, h, lead_ns);
    } else {
        fprintf(stderr, "otk-hard-probe: unknown mode %s\n", mode);
        rc = 2;
    }
    if (csv) {
        fclose(csv);
    }
    otk_hard_close(&link_);
    return rc;
}
