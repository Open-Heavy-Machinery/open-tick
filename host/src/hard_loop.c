/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file hard_loop.c
 * @brief The hard driver's step loop (opentick/host/hard_loop.h).
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE /* realpath */

#include <opentick/host/hard_loop.h>

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NOT_BEFORE_NS 200000u /* a planned send is at least this far ahead of now */

/* ---------------------------------------------------------------- small helpers */

static void set_err(char *err, size_t cap, const char *fmt, ...) {
    if (err == NULL || cap == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

static double wire_ns(uint32_t baud, size_t bytes) {
    return (double)bytes * 10.0 * 1e9 / (double)baud; /* 8N1: 10 bits a byte */
}

static size_t request_bytes(unsigned n) {
    return OTK_BRIDGE_FRAMING_BYTES + 5u + OTK_BRIDGE_SETPOINT_BYTES * n;
}

static size_t reply_bytes(unsigned n_cap) {
    return OTK_BRIDGE_FRAMING_BYTES + OTK_BRIDGE_REPLY_FIXED_BYTES + OTK_BRIDGE_CAPTURE_BYTES * n_cap;
}

static bool same_setpoint(const OtkBridgeSetpoint *a, const OtkBridgeSetpoint *b) {
    if (a->crank_phase_inc != b->crank_phase_inc) {
        return false;
    }
    for (unsigned i = 0; i < OTK_BRIDGE_ANALOG; i++) {
        if (a->analog[i] != b->analog[i]) {
            return false;
        }
    }
    return true;
}

static int cmp_u64(const void *a, const void *b) {
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* ---------------------------------------------------------------- results */

const char *otk_hard_result_name(OtkHardResult r) {
    switch (r) {
    case OTK_HARD_OK:
        return "ok";
    case OTK_HARD_DEGRADED:
        return "degraded";
    case OTK_HARD_ABORTED:
        return "aborted";
    default:
        return "?";
    }
}

void otk_hard_loop_defaults(OtkHardLoopConfig *c, uint32_t step_ticks) {
    memset(c, 0, sizeof(*c));
    c->baud = 3000000u;
    c->h = OTK_HARD_DEFAULT_H;
    c->margin_ns = OTK_HARD_DEFAULT_MARGIN_NS;
    c->spin_ns = OTK_HARD_DEFAULT_SPIN_NS;
    c->timeout_ns = 100000000u;
    c->retries = 3u;
    c->learn_ns = 2000000000u;
    c->cycle_guess_ns = 6000000.0;
    c->sync_bin_ns = 100000000u;
    c->drain_silence_ns = 300000000u;
    c->require_fresh = true;
    c->bridge.step_ticks = step_ticks;
    c->bridge.crank_teeth = 60u;
    c->bridge.crank_missing = 2u;
    c->bridge.crank_offset = 0u;
    c->bridge.cam_start = 0u;
    c->bridge.cam_end = 0x80000000u;
    c->bridge.capture_mask = 0x00u;
    c->bridge.capture_polarity = 0x3Fu;
    c->bridge.link_timeout_steps = 100u;
}

/* ---------------------------------------------------------------- REALTIME -> MONOTONIC */

static uint64_t clock_ns(clockid_t id) {
    struct timespec ts;
    clock_gettime(id, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static uint64_t system_clock(void *ctx, int id) {
    (void)ctx;
    return clock_ns((clockid_t)id);
}

void otk_rt_mono_init(OtkRtMono *m) {
    memset(m, 0, sizeof(*m));
}

static bool rt_mono_narrow(uint64_t rt_before, uint64_t rt_after) {
    return rt_after >= rt_before && rt_after - rt_before <= OTK_RT_MONO_MAX_WIDTH_NS;
}

bool otk_rt_mono_add(OtkRtMono *m, uint64_t rt_before, uint64_t mono, uint64_t rt_after) {
    if (!rt_mono_narrow(rt_before, rt_after)) {
        m->rejected++; /* interrupted, or REALTIME stepped between the reads: not a sample */
        return false;
    }
    const uint64_t half = (rt_after - rt_before) / 2u;
    const int64_t off = (int64_t)(rt_before + half - mono);
    if (!m->have) {
        m->offset_min = m->offset_max = off;
        m->have = true;
    }
    m->offset_ns = off;
    if (off < m->offset_min) {
        m->offset_min = off;
    }
    if (off > m->offset_max) {
        m->offset_max = off;
    }
    /* the half width counts up: the offset is good to that, either way */
    const uint64_t hw = (rt_after - rt_before + 1u) / 2u;
    if (hw > m->half_width_max_ns) {
        m->half_width_max_ns = hw;
    }
    m->samples++;
    return true;
}

void otk_rt_mono_sample_from(OtkRtMono *m, OtkRtMonoClock read, void *ctx) {
    uint64_t a = 0u, b = 0u, c = 0u;
    for (unsigned t = 0u; t < OTK_RT_MONO_TRIES; t++) {
        if (t > 0u) {
            m->retries++;
        }
        a = read(ctx, CLOCK_REALTIME);
        b = read(ctx, CLOCK_MONOTONIC);
        c = read(ctx, CLOCK_REALTIME);
        if (rt_mono_narrow(a, c)) {
            break;
        }
    }
    (void)otk_rt_mono_add(m, a, b, c); /* the last read; dropped and counted if still too wide */
}

void otk_rt_mono_sample(OtkRtMono *m) {
    otk_rt_mono_sample_from(m, system_clock, NULL);
}

uint64_t otk_rt_mono_to_mono(const OtkRtMono *m, uint64_t rt_ns) {
    return (uint64_t)((int64_t)rt_ns - m->offset_ns);
}

uint64_t otk_rt_mono_err_ns(const OtkRtMono *m) {
    if (!m->have) {
        return UINT64_MAX;
    }
    return m->half_width_max_ns + (uint64_t)(m->offset_max - m->offset_min);
}

/* ---------------------------------------------------------------- aiming */

static double ticks_per_ns(const OtkHardAim *a) {
    return (double)a->tick_hz / 1e9 * (1.0 + a->drift);
}

double otk_hard_aim_tick(const OtkHardAim *a, uint64_t host_ns) {
    const double dt = (double)(int64_t)(host_ns - a->t1);
    return (double)a->t2 + dt * ticks_per_ns(a);
}

uint32_t otk_hard_aim_step(const OtkHardAim *a, uint64_t host_ns) {
    const double tick = otk_hard_aim_tick(a, host_ns);
    if (tick <= (double)a->t_start) {
        return 0u;
    }
    const double k = floor((tick - (double)a->t_start) / (double)a->step_ticks);
    return k >= (double)UINT32_MAX ? UINT32_MAX : (uint32_t)k;
}

uint64_t otk_hard_aim_step_host_ns(const OtkHardAim *a, uint32_t step) {
    const double tick = (double)a->t_start + (double)step * (double)a->step_ticks;
    const double dt = (tick - (double)a->t2) / ticks_per_ns(a);
    return a->t1 + (uint64_t)(int64_t)llround(dt);
}

bool otk_hard_can_send_at(OtkSocketCan *s, const OtkCanFrame *f, uint64_t host_ns, uint64_t spin_ns, uint64_t *late_ns) {
    const uint64_t late = otk_hard_wait_until(host_ns, spin_ns);
    if (late_ns != NULL) {
        *late_ns = late;
    }
    return otk_socketcan_send(s, f);
}

/* Marks the run aborted; the first cause is kept, with what the last reply said. */
static void abort_run(OtkHardLoop *l, OtkHardAbort why) {
    if (l->stats.abort_reason == OTK_HARD_ABORT_NONE) {
        l->stats.abort_reason = why;
        l->stats.abort_computed = l->computed;
    }
    l->aborted = true;
}

const char *otk_hard_abort_name(OtkHardAbort a) {
    static const char *const names[] = { "none",  "io",   "no-reply", "bad-reply",      "crc",
                                         "bridge", "fill", "start",   "last-setpoints", "stop" };
    return (unsigned)a < sizeof(names) / sizeof(names[0]) ? names[a] : "?";
}

/* ---------------------------------------------------------------- one request, retried */

/* 0 with a reply; 1 if none came after every retransmission; -1 on an I/O error. */
static int xact(OtkHardLoop *l, uint8_t type, const uint8_t *p, size_t len, OtkHardReply *r) {
    for (unsigned attempt = 0; attempt <= l->cfg.retries; attempt++) {
        const int rc = otk_hard_transact(&l->link, type, p, len, attempt > 0, l->cfg.timeout_ns, r);
        if (rc <= 0) {
            return rc;
        }
    }
    return 1;
}

static void record_rtt(OtkHardLoop *l, uint64_t rtt) {
    if (l->n_rtt == l->cap_rtt) {
        const size_t cap = l->cap_rtt ? 2u * l->cap_rtt : 4096u;
        uint64_t *v = realloc(l->rtt, cap * sizeof(uint64_t));
        if (v == NULL) {
            return; /* the percentile then covers fewer exchanges */
        }
        l->rtt = v;
        l->cap_rtt = cap;
    }
    l->rtt[l->n_rtt++] = rtt;
}

static void refit_cycle(OtkHardLoop *l) {
    if (l->n_t4 < 16u) {
        return;
    }
    double p, t0, rms;
    const double guess = l->have_cycle ? l->cycle_p : l->cfg.cycle_guess_ns;
    if (otk_hard_cycle_fit(l->t4, l->n_t4, guess, &p, &t0, &rms)) {
        l->cycle_p = p;
        l->cycle_t0 = t0;
        l->have_cycle = true;
        l->stats.cycle_ns = p;
    }
}

static void push_t4(OtkHardLoop *l, uint64_t t4) {
    if (l->n_t4 == OTK_HARD_CYCLE_WINDOW) {
        memmove(l->t4, &l->t4[1], (OTK_HARD_CYCLE_WINDOW - 1u) * sizeof(uint64_t));
        l->n_t4--;
    }
    l->t4[l->n_t4++] = t4;
}

static void take_captures(OtkHardLoop *l, const OtkBridgeReply *d) {
    for (unsigned i = 0; i < d->n_cap; i++) {
        uint64_t tick;
        if (!otk_bridge_extend_tick(d->cap[i].start, d->t3, &tick)) {
            continue;
        }
        if (l->n_cap == OTK_HARD_CAPTURE_QUEUE) {
            l->cap_dropped++;
            continue;
        }
        OtkHardCapture *c = &l->cap[l->n_cap++];
        c->channel = d->cap[i].channel;
        c->start_tick = tick;
        c->width = d->cap[i].width;
        const uint64_t since = tick > l->aim.t_start ? tick - l->aim.t_start : 0u;
        const uint64_t step = since / l->cfg.bridge.step_ticks;
        c->step = step > UINT32_MAX ? UINT32_MAX : (uint32_t)step;
        if (l->computed > c->step && l->computed - c->step > l->stats.capture_lag_max) {
            l->stats.capture_lag_max = l->computed - c->step; /* the plant's step when it gets it, less the capture's */
        }
    }
}

/*
 * One exchange: the setpoints [first, first + n) from the ring. Every reply is
 * a timing sample; while running it is also checked and counted.
 * 0 on a reply; 1 if the run is aborted; -1 on an I/O or protocol error.
 */
static int exchange(OtkHardLoop *l, uint32_t first, unsigned n, OtkBridgeReply *d) {
    OtkBridgeRequest q;
    q.first_step = first;
    q.n = (uint8_t)n;
    for (unsigned i = 0; i < n; i++) {
        q.sp[i] = l->ring[(first + i) % OTK_HARD_RING];
    }
    uint8_t p[OTK_BRIDGE_MAX_PAYLOAD];
    const size_t len = otk_bridge_encode_request(&q, p, sizeof(p));
    OtkHardReply r;
    for (unsigned attempt = 0;; attempt++) {
        const int rc = xact(l, OTK_BRIDGE_EXCHANGE, p, len, &r);
        if (rc < 0) {
            abort_run(l, OTK_HARD_ABORT_IO);
            return -1;
        }
        if (rc > 0) {
            abort_run(l, OTK_HARD_ABORT_NO_REPLY); /* the bridge has timed the link out by now (§3.5) */
            return 1;
        }
        if (r.type != (OTK_BRIDGE_EXCHANGE | OTK_BRIDGE_REPLY) || !otk_bridge_decode_reply(r.payload, r.len, d)) {
            abort_run(l, OTK_HARD_ABORT_BAD_REPLY);
            return -1;
        }
        l->stats.abort_error = d->error;
        l->stats.abort_state = d->state;
        if ((d->error & OTK_BRIDGE_ERR_CRC) == 0u) {
            break;
        }
        /* the request was damaged on the way: nothing was applied, send it again */
        l->stats.crc_errors++;
        if (attempt >= l->cfg.retries) {
            abort_run(l, OTK_HARD_ABORT_CRC);
            return 1;
        }
    }
    l->stats.exchanges++;
    const OtkHardSample x = { r.t1, r.t4, d->t2, d->t3 };
    otk_hard_sync_add(&l->sync, &x);
    push_t4(l, r.t4);
    refit_cycle(l);
    l->last_n_cap = d->n_cap;
    l->aim.t1 = r.t1;
    l->aim.t2 = d->t2;
    l->aim.drift = l->sync.fitted ? l->sync.b : 0.0;

    if ((d->error & OTK_BRIDGE_ERR_ABORTED) != 0u || d->state == OTK_BRIDGE_ABORTED) {
        abort_run(l, OTK_HARD_ABORT_BRIDGE);
        return 1;
    }
    if ((d->error & OTK_BRIDGE_ERR_SEQUENCE) != 0u) {
        l->stats.seq_errors++; /* refused whole: the same setpoints go again next time */
    } else {
        l->queued = first + n;
    }
    const bool late = (d->error & OTK_BRIDGE_ERR_LATE) != 0u;
    const bool overflow = (d->error & OTK_BRIDGE_ERR_CAPTURE_OVERFLOW) != 0u;
    if (late) {
        l->stats.late += d->error_count; /* with an overflow too, the count is shared: counted as late */
    } else if (overflow) {
        l->stats.capture_lost += d->error_count;
    }
    if (!l->started) {
        return 0;
    }
    l->stats.underruns = d->underruns;
    record_rtt(l, r.t4 - r.t1);
    otk_rt_mono_sample(&l->rt);
    if (d->state == OTK_BRIDGE_RUNNING) {
        if (d->fill < l->stats.min_fill) {
            l->stats.min_fill = d->fill;
        }
        /* the applied setpoint is the one sent for its step (F-19), once that step had begun when the
         * request arrived (T2): before step 0 the outputs are at rest */
        const uint64_t begun = l->aim.t_start + (uint64_t)d->step * l->cfg.bridge.step_ticks;
        if (l->stats.underruns == 0u && l->stats.late == 0u && d->t2 >= begun && d->step < l->queued &&
            d->step + OTK_HARD_RING > l->computed) {
            l->stats.applied_checked++;
            if (!same_setpoint(&d->applied, &l->ring[d->step % OTK_HARD_RING])) {
                l->stats.applied_wrong++;
            }
        }
    }
    if (l->sync.fitted) {
        const double b = otk_hard_sync_bound_ns(&l->sync);
        if (b > l->stats.bound_max_ns) {
            l->stats.bound_max_ns = b;
        }
        l->stats.bound_end_ns = b;
    }
    take_captures(l, d);
    return 0;
}

/* ---------------------------------------------------------------- open */

static int control(OtkHardLoop *l, uint8_t type, const uint8_t *p, size_t len, OtkHardReply *r) {
    const int rc = xact(l, type, p, len, r);
    if (rc != 0) {
        return -1;
    }
    return r->type == (uint8_t)(type | OTK_BRIDGE_REPLY) ? 0 : -1;
}

int otk_hard_loop_open(OtkHardLoop *l, const char *dev, const OtkHardLoopConfig *cfg, char *err, size_t err_cap) {
    memset(l, 0, sizeof(*l));
    l->link.fd = -1;
    l->cfg = *cfg;
    l->stats.min_fill = UINT16_MAX;
    otk_rt_mono_init(&l->rt);
    if (cfg->h == 0u || cfg->h > OTK_HARD_MAX_H) {
        set_err(err, err_cap, "H must be 1..%u steps, not %u", OTK_HARD_MAX_H, cfg->h);
        return -1;
    }
    if (cfg->bridge.step_ticks == 0u) {
        set_err(err, err_cap, "the step must be at least one tick");
        return -1;
    }
    if (otk_hard_open(&l->link, dev, cfg->baud) != 0) {
        set_err(err, err_cap, "cannot open %s at %u baud", dev, cfg->baud);
        return -1;
    }
    (void)otk_hard_drain(&l->link, cfg->drain_silence_ns, 10u * cfg->drain_silence_ns);

    OtkHardReply r;
    OtkBridgeReply d;
    /* §8 step 1: a freshly programmed bridge is idle, its counters at zero (F-17) */
    if (cfg->require_fresh) {
        const uint8_t empty[5] = { 0, 0, 0, 0, 0 };
        if (xact(l, OTK_BRIDGE_EXCHANGE, empty, sizeof(empty), &r) != 0 ||
            r.type != (OTK_BRIDGE_EXCHANGE | OTK_BRIDGE_REPLY) || !otk_bridge_decode_reply(r.payload, r.len, &d)) {
            set_err(err, err_cap, "no reply from the bridge on %s", dev);
            otk_hard_close(&l->link);
            return -1;
        }
        if (d.state != OTK_BRIDGE_IDLE) {
            set_err(err, err_cap, "the bridge is not freshly programmed (state %u, not idle): load the bitstream first",
                    (unsigned)d.state);
            otk_hard_close(&l->link);
            return -1;
        }
    }
    /* §8 step 2: configure, and check what the bridge has */
    uint8_t p[32];
    const size_t len = otk_bridge_encode_configure(&cfg->bridge, p, sizeof(p));
    if (len == 0u || control(l, OTK_BRIDGE_CONFIGURE, p, len, &r) != 0 ||
        !otk_bridge_decode_configure_reply(r.payload, r.len, &l->caps)) {
        set_err(err, err_cap, "no reply to configure");
        otk_hard_close(&l->link);
        return -1;
    }
    if (l->caps.status != OTK_BRIDGE_STATUS_OK) {
        set_err(err, err_cap, "the bridge refused the configuration (status %u)", (unsigned)l->caps.status);
        otk_hard_close(&l->link);
        return -1;
    }
    if ((cfg->need_tick_hz != 0u && l->caps.tick_hz != cfg->need_tick_hz) || l->caps.tick_hz == 0u ||
        l->caps.n_analog < cfg->need_analog || l->caps.n_capture < cfg->need_capture) {
        set_err(err, err_cap,
                "the bridge has a %u Hz tick, %u analog outputs and %u capture channels; the run needs %u Hz, %u and %u",
                l->caps.tick_hz, (unsigned)l->caps.n_analog, (unsigned)l->caps.n_capture, cfg->need_tick_hz,
                (unsigned)cfg->need_analog, (unsigned)cfg->need_capture);
        otk_hard_close(&l->link);
        return -1;
    }
    l->aim.tick_hz = l->caps.tick_hz;
    l->aim.step_ticks = cfg->bridge.step_ticks;
    otk_hard_sync_init(&l->sync, l->caps.tick_hz, cfg->sync_bin_ns);

    /* learn the forwarding cycle and the mapping, back to back */
    const uint64_t until = otk_hard_now_ns() + cfg->learn_ns;
    while (otk_hard_now_ns() < until) {
        const int rc = exchange(l, 0u, 0u, &d);
        if (rc != 0) {
            set_err(err, err_cap, "the bridge stopped answering while the link was learnt");
            otk_hard_close(&l->link);
            return -1;
        }
    }
    l->stats.exchanges = 0u;
    l->aborted = false;
    return 0;
}

/* ---------------------------------------------------------------- the loop */

void otk_hard_loop_set(OtkHardLoop *l, const OtkBridgeSetpoint *sp) {
    l->pending = *sp;
    l->have_pending = true;
}

uint32_t otk_hard_loop_computed(const OtkHardLoop *l) {
    return l->computed;
}

size_t otk_hard_loop_captures(OtkHardLoop *l, OtkHardCapture *out, size_t cap) {
    const size_t n = l->n_cap < cap ? l->n_cap : cap;
    memcpy(out, l->cap, n * sizeof(OtkHardCapture));
    memmove(l->cap, &l->cap[n], (l->n_cap - n) * sizeof(OtkHardCapture));
    l->n_cap -= n;
    return n;
}

void otk_hard_loop_attach_can(OtkHardLoop *l, OtkSocketCan *s, OtkHardCanSentFn sent, void *ctx) {
    l->can = s;
    l->can_sent = sent;
    l->can_ctx = ctx;
}

bool otk_hard_loop_can_at(OtkHardLoop *l, const OtkCanFrame *f, uint32_t step) {
    if (l->n_can == OTK_HARD_CAN_QUEUE) {
        l->can_overflows++;
        return false;
    }
    l->can_q[l->n_can].frame = *f;
    l->can_q[l->n_can].step = step;
    l->n_can++;
    return true;
}

/* Sends, in step order, every queued CAN frame whose step begins before `until`. */
static void service_can(OtkHardLoop *l, uint64_t until) {
    while (l->n_can > 0u && l->can != NULL && l->started) {
        size_t first = 0;
        for (size_t i = 1; i < l->n_can; i++) {
            if (l->can_q[i].step < l->can_q[first].step) {
                first = i;
            }
        }
        const uint64_t due = otk_hard_aim_step_host_ns(&l->aim, l->can_q[first].step);
        if (due > until) {
            return;
        }
        OtkHardCanSent sent;
        sent.frame = l->can_q[first].frame;
        sent.step = l->can_q[first].step;
        sent.planned_ns = due;
        (void)otk_hard_wait_until(due, l->cfg.spin_ns);
        sent.ok = otk_socketcan_send(l->can, &sent.frame);
        sent.sent_ns = otk_hard_now_ns();
        memmove(&l->can_q[first], &l->can_q[first + 1u], (l->n_can - first - 1u) * sizeof(OtkHardCanItem));
        l->n_can--;
        if (l->can_sent != NULL) {
            l->can_sent(l->can_ctx, &sent);
        }
    }
}

/* How long before a forwarding instant a request is written: the margin plus the bytes' wire time (§4.1, F-29). */
static double lead_ns(const OtkHardLoop *l) {
    const double step_ns = (double)l->cfg.bridge.step_ticks * 1e9 / (double)l->caps.tick_hz;
    double n_guess = ceil((l->have_cycle ? l->cycle_p : l->cfg.cycle_guess_ns) / step_ns) + 1.0;
    if (n_guess > (double)OTK_BRIDGE_MAX_SETPOINTS) {
        n_guess = (double)OTK_BRIDGE_MAX_SETPOINTS;
    }
    return (double)l->cfg.margin_ns + wire_ns(l->cfg.baud, request_bytes((unsigned)n_guess) + reply_bytes(l->last_n_cap));
}

/* The next send time: written so that its bytes and the reply's are on the wire, and the margin left, before a forwarding instant (§4.1). */
static uint64_t next_plan(const OtkHardLoop *l, uint64_t now) {
    const uint64_t not_before = now + NOT_BEFORE_NS;
    if (!l->have_cycle) {
        /* a link without a cycle found: one exchange per guessed cycle */
        const uint64_t at = l->aim.t1 + (uint64_t)l->cfg.cycle_guess_ns;
        return at > not_before ? at : not_before;
    }
    const double lead = lead_ns(l);
    const uint64_t t0 = (uint64_t)l->cycle_t0;
    const double from = (double)(int64_t)(not_before - t0) + lead;
    const double k = ceil(from / l->cycle_p);
    return t0 + (uint64_t)(int64_t)llround(k * l->cycle_p - lead);
}

/* When the next queued CAN frame is due, or UINT64_MAX. */
static uint64_t next_can_due(const OtkHardLoop *l) {
    if (l->can == NULL || l->n_can == 0u || !l->started) {
        return UINT64_MAX;
    }
    uint32_t first = l->can_q[0].step;
    for (size_t i = 1; i < l->n_can; i++) {
        if (l->can_q[i].step < first) {
            first = l->can_q[i].step;
        }
    }
    return otk_hard_aim_step_host_ns(&l->aim, first);
}

static bool fill_and_start(OtkHardLoop *l) {
    OtkBridgeReply d;
    if (exchange(l, 0u, l->cfg.h, &d) != 0 || d.error != 0u || l->queued != l->cfg.h) {
        abort_run(l, OTK_HARD_ABORT_FILL);
        return false;
    }
    OtkHardReply r;
    OtkBridgeStartReply sr;
    if (control(l, OTK_BRIDGE_START, NULL, 0u, &r) != 0 || !otk_bridge_decode_start_reply(r.payload, r.len, &sr) ||
        sr.status != OTK_BRIDGE_STATUS_OK) {
        abort_run(l, OTK_HARD_ABORT_START);
        return false;
    }
    l->aim.t_start = sr.t_start;
    l->started = true;
    /* The first exchange after the start never gives way to a CAN frame: the buffer holds only
     * the H setpoints of the fill, and the start's own reply may have waited a whole cycle. Frames
     * queued during the fill for the first steps go out after it, late (2026-11-28: with 1 ms
     * steps and a TSC1 queued for step 0, giving way here cost one underrun a run). */
    l->can_skipped = true;
    return true;
}

/*
 * One scheduled exchange with every setpoint computed since the last one.
 * The host is blocked while it waits for the reply (about the lead, until the
 * forwarding instant), so a CAN frame due in that window would go out late.
 * Such a frame is sent at its time instead and the exchange moves to the next
 * cycle; never two cycles in a row, so the link is not starved (a frame then
 * goes out late, and the budget's can_tx_err shows it).
 */
static bool scheduled_exchange(OtkHardLoop *l, uint64_t plan) {
    service_can(l, plan);
    const uint64_t busy_until = plan + (uint64_t)lead_ns(l) + NOT_BEFORE_NS;
    if (!l->can_skipped && next_can_due(l) < busy_until) {
        service_can(l, busy_until);
        l->can_skipped = true;
        return true;
    }
    l->can_skipped = false;
    const uint64_t late = otk_hard_wait_until(plan, l->cfg.spin_ns);
    if (late > l->stats.send_late_max_ns) {
        l->stats.send_late_max_ns = late;
    }
    uint32_t n = l->computed - l->queued;
    if (n > OTK_BRIDGE_MAX_SETPOINTS) {
        n = OTK_BRIDGE_MAX_SETPOINTS;
    }
    OtkBridgeReply d;
    const int rc = exchange(l, l->queued, n, &d);
    if (rc != 0) {
        abort_run(l, OTK_HARD_ABORT_IO); /* the exchange set its own cause first */
        return false;
    }
    return true;
}

bool otk_hard_loop_next(OtkHardLoop *l) {
    if (l->aborted) {
        return false;
    }
    /* the step just computed; without a setpoint, the last one is repeated */
    const OtkBridgeSetpoint sp = l->have_pending ? l->pending
                                 : l->computed > 0u ? l->ring[(l->computed - 1u) % OTK_HARD_RING]
                                                    : (OtkBridgeSetpoint){ 0u, { 0u, 0u, 0u, 0u } };
    l->ring[l->computed % OTK_HARD_RING] = sp;
    l->computed++;
    l->have_pending = false;
    if (!l->started) {
        if (l->computed < l->cfg.h) {
            return true;
        }
        if (!fill_and_start(l)) {
            return false;
        }
    }
    for (;;) {
        const uint64_t plan = next_plan(l, otk_hard_now_ns());
        const uint32_t target = otk_hard_aim_step(&l->aim, plan) + l->cfg.h + 1u;
        if (l->computed < target && l->computed - l->queued < OTK_BRIDGE_MAX_SETPOINTS) {
            return true; /* the plant is less than H ahead at the next exchange: one more step first */
        }
        if (!scheduled_exchange(l, plan)) {
            return false;
        }
    }
}

OtkHardResult otk_hard_loop_stop(OtkHardLoop *l) {
    if (l->started && !l->aborted) {
        /* Keep exchanging until the bridge is half-way through the application's
         * last step, sending the CAN frames still due; then stop. The last
         * setpoint is queued again a few times behind it, so a stop that
         * arrives a step or two late finds the buffer full rather than
         * counting an underrun the run did not have: the bridge holds that
         * setpoint either way. */
        const uint32_t last = l->computed - 1u;
        const OtkBridgeSetpoint hold = l->ring[last % OTK_HARD_RING];
        for (unsigned i = 0; i < l->cfg.h && l->computed - l->queued < OTK_BRIDGE_MAX_SETPOINTS; i++) {
            l->ring[l->computed % OTK_HARD_RING] = hold;
            l->computed++;
        }
        const uint64_t half_step = (uint64_t)((double)l->cfg.bridge.step_ticks * 0.5e9 / (double)l->caps.tick_hz);
        const uint64_t give_up = otk_hard_aim_step_host_ns(&l->aim, last) + 2000000000u;
        uint64_t last_at = 0;
        for (;;) {
            const uint64_t now = otk_hard_now_ns();
            const uint64_t plan = next_plan(l, now);
            last_at = otk_hard_aim_step_host_ns(&l->aim, last) + half_step;
            if (l->computed == l->queued && plan >= last_at) {
                break;
            }
            if (now > give_up || !scheduled_exchange(l, plan)) {
                abort_run(l, OTK_HARD_ABORT_LAST_SETPOINTS); /* unless the exchange set its own cause first */
                break;
            }
        }
        if (!l->aborted) {
            service_can(l, last_at);
            (void)otk_hard_wait_until(last_at, l->cfg.spin_ns);
            OtkHardReply r;
            OtkBridgeStopReply st;
            if (control(l, OTK_BRIDGE_STOP, NULL, 0u, &r) == 0 && otk_bridge_decode_stop_reply(r.payload, r.len, &st)) {
                l->stats.steps = st.step;
                l->stats.underruns = st.underruns;
            } else {
                abort_run(l, OTK_HARD_ABORT_STOP);
            }
        }
    }
    l->stats.timeouts = l->link.timeouts;
    otk_hard_close(&l->link);
    if (l->aborted || !l->started) {
        return OTK_HARD_ABORTED;
    }
    if (l->stats.underruns != 0u || l->stats.late != 0u || l->stats.applied_wrong != 0u) {
        return OTK_HARD_DEGRADED;
    }
    return OTK_HARD_OK;
}

bool otk_hard_loop_bridge_ns(const OtkHardLoop *l, uint64_t host_ns, int64_t *bridge_ns) {
    if (!l->started || !l->sync.fitted) {
        return false;
    }
    const double tick = otk_hard_sync_to_bridge(&l->sync, host_ns);
    const double ns = (tick - (double)l->aim.t_start) * 1e9 / (double)l->caps.tick_hz;
    *bridge_ns = (int64_t)llround(ns);
    return true;
}

bool otk_hard_loop_bridge_ns_rt(const OtkHardLoop *l, uint64_t realtime_ns, int64_t *bridge_ns) {
    if (!l->rt.have) {
        return false;
    }
    return otk_hard_loop_bridge_ns(l, otk_rt_mono_to_mono(&l->rt, realtime_ns), bridge_ns);
}

uint64_t otk_hard_loop_bus_err_ns(const OtkHardLoop *l) {
    const uint64_t rt = otk_rt_mono_err_ns(&l->rt);
    if (rt == UINT64_MAX || !isfinite(l->stats.bound_max_ns)) {
        return UINT64_MAX;
    }
    return (uint64_t)ceil(l->stats.bound_max_ns) + rt;
}

uint64_t otk_hard_loop_rtt_pct(const OtkHardLoop *l, double p) {
    if (l->n_rtt == 0u) {
        return 0u;
    }
    uint64_t *v = malloc(l->n_rtt * sizeof(uint64_t));
    if (v == NULL) {
        return 0u;
    }
    memcpy(v, l->rtt, l->n_rtt * sizeof(uint64_t));
    qsort(v, l->n_rtt, sizeof(uint64_t), cmp_u64);
    size_t i = (size_t)ceil(p / 100.0 * (double)l->n_rtt); /* nearest rank */
    if (i == 0u) {
        i = 1u;
    }
    if (i > l->n_rtt) {
        i = l->n_rtt;
    }
    const uint64_t x = v[i - 1u];
    free(v);
    return x;
}

void otk_hard_loop_free(OtkHardLoop *l) {
    free(l->rtt);
    l->rtt = NULL;
    l->n_rtt = l->cap_rtt = 0u;
    otk_hard_close(&l->link);
}

/* ---------------------------------------------------------------- the budget file */

void otk_hard_budget_none(OtkHardBudget *b) {
    b->underruns = b->late = b->rtt_p99_ns = b->mapping_bound_ns = b->realtime_err_ns = b->can_tx_err_ns = UINT64_MAX;
}

static bool meta_ok(const char *s) {
    return s == NULL || (strpbrk(s, ";\r\n") == NULL);
}

/* One row: quantity, declared (UINT64_MAX: none), measured (UINT64_MAX: none), unit; within when both are there. */
static void row(FILE *fp, const char *q, uint64_t declared, uint64_t measured, const char *unit) {
    fprintf(fp, "%s,", q);
    if (declared != UINT64_MAX) {
        fprintf(fp, "%llu", (unsigned long long)declared);
    }
    fputc(',', fp);
    if (measured != UINT64_MAX) {
        fprintf(fp, "%llu", (unsigned long long)measured);
    }
    fprintf(fp, ",%s,", unit);
    if (declared != UINT64_MAX && measured != UINT64_MAX) {
        fputs(measured <= declared ? "yes" : "no", fp);
    }
    fputc('\n', fp);
}

uint64_t otk_hard_dut_to_plant_ns(unsigned h, uint64_t step_ns, uint64_t cycle_ns, uint64_t rtt_ns) {
    return ((uint64_t)h + 1u) * step_ns + 2u * cycle_ns + rtt_ns;
}

static uint64_t ceil_u64(double x) {
    return isfinite(x) && x >= 0.0 ? (uint64_t)ceil(x) : UINT64_MAX;
}

bool otk_hard_budget_write(FILE *fp, const OtkHardLoop *l, OtkHardResult result, const OtkHardBudget *declared,
                           const OtkHardRunMeta *meta, const OtkHardCanErr *can) {
    const OtkHardRunMeta none = { NULL, NULL, NULL, NULL, NULL };
    if (meta == NULL) {
        meta = &none;
    }
    if (!meta_ok(meta->link) || !meta_ok(meta->usb) || !meta_ok(meta->bitstream_md5) || !meta_ok(meta->commit) ||
        !meta_ok(meta->source) || l->caps.tick_hz == 0u) {
        return false;
    }
    const uint64_t step_ns = (uint64_t)llround((double)l->cfg.bridge.step_ticks * 1e9 / (double)l->caps.tick_hz);
    const uint64_t lead_ns = step_ns * l->cfg.h;
    char why[96] = "";
    if (l->stats.abort_reason != OTK_HARD_ABORT_NONE) {
        snprintf(why, sizeof(why), "; abort=%s; abort_computed=%u; abort_error=0x%04x; abort_state=%u",
                 otk_hard_abort_name(l->stats.abort_reason), l->stats.abort_computed, l->stats.abort_error,
                 l->stats.abort_state);
    }
    fprintf(fp, "# otk-hard-budget 1; result=%s%s; H=%u; step_ns=%llu; margin_ns=%llu; tick_hz=%u; link=%s; usb=%s; "
                "bitstream_md5=%s; commit=%s; source=%s\n",
            otk_hard_result_name(result), why, l->cfg.h, (unsigned long long)step_ns, (unsigned long long)l->cfg.margin_ns,
            l->caps.tick_hz, meta->link ? meta->link : "", meta->usb ? meta->usb : "",
            meta->bitstream_md5 ? meta->bitstream_md5 : "", meta->commit ? meta->commit : "",
            meta->source ? meta->source : "");
    fputs("quantity,declared,measured,unit,within\n", fp);
    const uint64_t p50 = l->n_rtt ? otk_hard_loop_rtt_pct(l, 50.0) : UINT64_MAX;
    const uint64_t p99 = l->n_rtt ? otk_hard_loop_rtt_pct(l, 99.0) : UINT64_MAX;
    const uint64_t pmax = l->n_rtt ? otk_hard_loop_rtt_pct(l, 100.0) : UINT64_MAX;
    row(fp, "steps", UINT64_MAX, l->stats.steps, "steps");
    row(fp, "exchanges", UINT64_MAX, l->stats.exchanges, "exchanges");
    row(fp, "underruns", declared->underruns, l->stats.underruns, "steps");
    row(fp, "late_setpoints", declared->late, l->stats.late, "setpoints");
    row(fp, "applied_wrong", 0u, l->stats.applied_wrong, "setpoints");
    row(fp, "applied_checked", UINT64_MAX, l->stats.applied_checked, "setpoints");
    row(fp, "timeouts", UINT64_MAX, l->stats.timeouts, "attempts");
    row(fp, "min_fill", UINT64_MAX, l->stats.min_fill == UINT16_MAX ? UINT64_MAX : l->stats.min_fill, "setpoints");
    row(fp, "plant_lead_ns", lead_ns, lead_ns, "ns");
    row(fp, "rtt_p50_ns", UINT64_MAX, p50, "ns");
    row(fp, "rtt_p99_ns", declared->rtt_p99_ns, p99, "ns");
    row(fp, "rtt_max_ns", UINT64_MAX, pmax, "ns");
    /* declared with the guessed cycle; the bound with the fitted one; and the delay the captures had */
    const uint64_t guess_cycle = ceil_u64(l->cfg.cycle_guess_ns);
    const uint64_t fit_cycle = ceil_u64(l->stats.cycle_ns > 0.0 ? l->stats.cycle_ns : l->cfg.cycle_guess_ns);
    const uint64_t declared_delay = declared->rtt_p99_ns == UINT64_MAX
                                        ? UINT64_MAX
                                        : otk_hard_dut_to_plant_ns(l->cfg.h, step_ns, guess_cycle, declared->rtt_p99_ns);
    row(fp, "dut_to_plant_delay_ns", declared_delay,
        p99 == UINT64_MAX ? UINT64_MAX : otk_hard_dut_to_plant_ns(l->cfg.h, step_ns, fit_cycle, p99), "ns");
    row(fp, "capture_lag_max_ns", declared_delay,
        l->stats.capture_lag_max == 0u ? UINT64_MAX : (uint64_t)l->stats.capture_lag_max * step_ns, "ns");
    row(fp, "mapping_bound_ns", declared->mapping_bound_ns, ceil_u64(l->stats.bound_max_ns), "ns");
    row(fp, "realtime_to_monotonic_err_ns", declared->realtime_err_ns, otk_rt_mono_err_ns(&l->rt), "ns");
    row(fp, "realtime_to_monotonic_rejected", UINT64_MAX, l->rt.rejected, "samples");
    row(fp, "bus_clock_err_ns", declared->mapping_bound_ns == UINT64_MAX || declared->realtime_err_ns == UINT64_MAX
                                    ? UINT64_MAX
                                    : declared->mapping_bound_ns + declared->realtime_err_ns,
        otk_hard_loop_bus_err_ns(l), "ns");
    row(fp, "send_late_max_ns", UINT64_MAX, l->stats.send_late_max_ns, "ns");
    row(fp, "cycle_ns", UINT64_MAX, ceil_u64(l->stats.cycle_ns), "ns");
    const bool have_can = can != NULL && can->n > 0u;
    row(fp, "can_frames", UINT64_MAX, have_can ? can->n : UINT64_MAX, "frames");
    row(fp, "can_tx_err_p50_ns", UINT64_MAX, have_can ? can->p50_ns : UINT64_MAX, "ns");
    row(fp, "can_tx_err_p99_ns", declared->can_tx_err_ns, have_can ? can->p99_ns : UINT64_MAX, "ns");
    row(fp, "can_tx_err_max_ns", UINT64_MAX, have_can ? can->max_ns : UINT64_MAX, "ns");
    return ferror(fp) == 0;
}

/* ---------------------------------------------------------------- the USB path */

static bool read_line(const char *path, char *out, size_t cap) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return false;
    }
    const bool ok = fgets(out, (int)cap, f) != NULL;
    fclose(f);
    if (ok) {
        out[strcspn(out, "\r\n")] = '\0';
    }
    return ok;
}

bool otk_hard_usb_path(const char *dev, char *out, size_t cap) {
    snprintf(out, cap, "none");
    char real[PATH_MAX];
    if (realpath(dev, real) == NULL) {
        return false;
    }
    const char *name = strrchr(real, '/');
    name = name ? name + 1 : real;
    char sys[PATH_MAX + 64];
    snprintf(sys, sizeof(sys), "/sys/class/tty/%s/device", name);
    char dir[PATH_MAX];
    if (realpath(sys, dir) == NULL) {
        return false;
    }
    /* walk up to the USB device: the first directory with a busnum */
    for (;;) {
        char probe[PATH_MAX + 16];
        char speed[32];
        snprintf(probe, sizeof(probe), "%s/busnum", dir);
        char tmp[32];
        if (read_line(probe, tmp, sizeof(tmp))) {
            snprintf(probe, sizeof(probe), "%s/speed", dir);
            if (!read_line(probe, speed, sizeof(speed))) {
                snprintf(speed, sizeof(speed), "?");
            }
            const char *base = strrchr(dir, '/');
            snprintf(out, cap, "%s@%sM", base ? base + 1 : dir, speed);
            return true;
        }
        char *slash = strrchr(dir, '/');
        if (slash == NULL || slash == dir) {
            return false;
        }
        *slash = '\0';
    }
}
