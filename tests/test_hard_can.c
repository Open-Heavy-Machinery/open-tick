/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_hard_can.c
 * @brief CAN on the bridge's clock (docs/bridge-protocol.md §5) on a virtual
 *        bus: frames sent at the instant a step begins, read back with the
 *        kernel's timestamps (CLOCK_REALTIME) and mapped REALTIME ->
 *        MONOTONIC -> bridge. First on a synthetic mapping, then through the
 *        whole loop against the fake bridge.
 *
 * Needs a CAN interface: vcan0, or the one OTK_TEST_CAN_IF names. Skipped
 * (exit 77) without one, like test_socketcan. Without root:
 *
 *     unshare -rn sh -c 'ip link add dev vcan0 type vcan && ip link set up vcan0 && exec build/tests/test_hard_can'
 */

#define _GNU_SOURCE

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "fake_bridge.h"
#include <opentick/host/hard_loop.h>
#include <opentick/host/socketcan.h>

#define TOL_NS 2000000.0 /* a loaded CI runner can wake 1-2 ms late; typical is tens of us */

static const char *ifname(void) {
    const char *e = getenv("OTK_TEST_CAN_IF");
    return (e != NULL && e[0] != '\0') ? e : "vcan0";
}

static bool open_can(OtkSocketCan *s) {
    if (!otk_socketcan_open(s, ifname())) {
        return false;
    }
    if (!otk_socketcan_enable_timestamps(s, true)) {
        otk_socketcan_close(s);
        return false;
    }
    return true;
}

static OtkCanFrame frame_of(uint32_t k) {
    OtkCanFrame f;
    memset(&f, 0, sizeof(f));
    f.id = 0x100u;
    f.len = 4u;
    memcpy(f.data, &k, 4u);
    return f;
}

/* Reads every frame back; for each, its step (from the payload) and kernel time. */
static size_t read_back(OtkSocketCan *s, uint32_t *step, uint64_t *rt, size_t cap) {
    size_t n = 0;
    for (int idle = 0; idle < 50 && n < cap;) {
        OtkCanFrame f;
        OtkSocketCanRxInfo info;
        const int r = otk_socketcan_recv_ex(s, &f, &info);
        if (r == 1 && info.own && info.has_time && f.id == 0x100u) {
            memcpy(&step[n], f.data, 4u);
            rt[n] = info.time_ns;
            n++;
            idle = 0;
        } else if (r == 0) {
            otk_hard_sleep_until(otk_hard_now_ns() + 1000000u);
            idle++;
        }
    }
    return n;
}

static void test_send_at_on_a_synthetic_mapping(void) {
    OtkSocketCan s;
    CHECK(open_can(&s));
    /* A bridge whose step 0 begins 30 ms from now, 10 ms steps, 7 ppm fast. */
    const uint64_t now = otk_hard_now_ns();
    const OtkHardAim a = { 27000000u, 270000u, 9000000000u + 810000u, now, 9000000000u, 7e-6 };
    OtkRtMono rt;
    otk_rt_mono_init(&rt);
    enum { N = 20 };
    uint64_t late_max = 0;
    for (uint32_t k = 0; k < N; k++) {
        const OtkCanFrame f = frame_of(k);
        uint64_t late = 0;
        CHECK(otk_hard_can_send_at(&s, &f, otk_hard_aim_step_host_ns(&a, k), OTK_HARD_DEFAULT_SPIN_NS, &late));
        otk_rt_mono_sample(&rt);
        late_max = late > late_max ? late : late_max;
    }
    uint32_t step[N];
    uint64_t t[N];
    const size_t n = read_back(&s, step, t, N);
    CHECK(n == N);
    double worst = 0;
    for (size_t i = 0; i < n; i++) {
        /* REALTIME -> MONOTONIC -> bridge, against the step's beginning */
        const double tick = otk_hard_aim_tick(&a, otk_rt_mono_to_mono(&rt, t[i]));
        const double want = (double)a.t_start + (double)step[i] * 270000.0;
        const double err_ns = fabs(tick - want) / 0.027;
        worst = err_ns > worst ? err_ns : worst;
    }
    printf("  synthetic mapping: %zu frames, worst |t_bus - step start| %.1f us, sends at most %.1f us late, "
           "REALTIME -> MONOTONIC error %llu ns\n",
           n, worst / 1e3, (double)late_max / 1e3, (unsigned long long)otk_rt_mono_err_ns(&rt));
    CHECK(worst < TOL_NS);
    CHECK(otk_rt_mono_err_ns(&rt) < 1000000u);
    otk_socketcan_close(&s);
}

typedef struct {
    unsigned n, failed;
    uint64_t late_max;
} Sent;

static void on_sent(void *ctx, const OtkHardCanSent *x) {
    Sent *s = ctx;
    s->n++;
    s->failed += !x->ok;
    const uint64_t late = x->sent_ns > x->planned_ns ? x->sent_ns - x->planned_ns : 0u;
    s->late_max = late > s->late_max ? late : s->late_max;
}

static void test_loop_sends_at_the_step(void) {
    OtkSocketCan s;
    CHECK(open_can(&s));
    static FakeBridge f;
    fake_init(&f);
    f.drift = -6e-6;
    char name[256], err[256] = "";
    CHECK(fake_start(&f, name, sizeof(name)) != NULL);
    OtkHardLoopConfig c;
    otk_hard_loop_defaults(&c, 135000u); /* 5 ms */
    c.learn_ns = 300000000u;
    c.cycle_guess_ns = 2000000.0;
    c.drain_silence_ns = 50000000u;
    c.sync_bin_ns = 20000000u;
    static OtkHardLoop hl;
    CHECK(otk_hard_loop_open(&hl, name, &c, err, sizeof(err)) == 0);
    Sent sent = { 0u, 0u, 0u };
    otk_hard_loop_attach_can(&hl, &s, on_sent, &sent);
    enum { STEPS = 100 };
    for (uint32_t k = 0; k < STEPS; k++) {
        const OtkCanFrame fr = frame_of(k);
        CHECK(otk_hard_loop_can_at(&hl, &fr, k));
        const OtkBridgeSetpoint sp = { 1000u + k, { 0u, 0u, 0u, 0u } };
        otk_hard_loop_set(&hl, &sp);
        CHECK(otk_hard_loop_next(&hl));
    }
    CHECK(otk_hard_loop_stop(&hl) == OTK_HARD_OK);
    fake_stop(&f);
    CHECK(sent.n == STEPS && sent.failed == 0u);
    uint32_t step[STEPS];
    uint64_t t[STEPS];
    const size_t n = read_back(&s, step, t, STEPS);
    CHECK(n == STEPS);
    double worst = 0;
    unsigned mapped = 0;
    for (size_t i = 0; i < n; i++) {
        int64_t bus;
        if (otk_hard_loop_bridge_ns_rt(&hl, t[i], &bus)) {
            mapped++;
            const double e = fabs((double)bus - (double)step[i] * 5e6);
            worst = e > worst ? e : worst;
        }
    }
    const uint64_t bound = otk_hard_loop_bus_err_ns(&hl);
    printf("  through the loop: %u frames mapped, worst |t_bus - t| %.1f us, bus_clock_err_ns %llu, sends at most "
           "%.1f us late\n",
           mapped, worst / 1e3, (unsigned long long)bound, (double)sent.late_max / 1e3);
    CHECK(mapped == STEPS);
    CHECK(worst < TOL_NS);
    CHECK(bound != UINT64_MAX && bound > 0u);
    otk_hard_loop_free(&hl);
    otk_socketcan_close(&s);
}

int main(void) {
    OtkSocketCan probe;
    if (!otk_socketcan_open(&probe, ifname())) {
        printf("skip: no CAN interface %s (%s)\n", ifname(), strerror(errno));
        return 77;
    }
    otk_socketcan_close(&probe);
    RUN_TEST(test_send_at_on_a_synthetic_mapping);
    RUN_TEST(test_loop_sends_at_the_step);
    return check_report();
}
