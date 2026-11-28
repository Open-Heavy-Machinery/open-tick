/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_hard_loop.c
 * @brief The hard driver's step loop (opentick/host/hard_loop.h) without a
 *        board, against a fake bridge on a pseudo-terminal (fake_bridge.h):
 *        the fill, the start, every setpoint applied at its own step, an
 *        underrun, a late setpoint and a wrong applied setpoint (degraded),
 *        a silent bridge (aborted),
 *        the stop, a capture delivered with its step, a bridge that is not
 *        fresh, missing capabilities; and the pieces: aiming at a step, the
 *        REALTIME -> MONOTONIC step, the budget file, the USB path.
 */

#define _GNU_SOURCE

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "fake_bridge.h"
#include <opentick/host/driver.h>
#include <opentick/host/hard_loop.h>

#define STEP_TICKS 135000u /* 5 ms: roomy enough for a loaded CI runner */
#define STEPS 120u

static OtkBridgeSetpoint sp_of(uint32_t k) {
    OtkBridgeSetpoint sp;
    sp.crank_phase_inc = 1000u + k;
    sp.analog[0] = (uint16_t)k;
    sp.analog[1] = (uint16_t)(k * 3u);
    sp.analog[2] = 7u;
    sp.analog[3] = 9u;
    return sp;
}

static void config(OtkHardLoopConfig *c) {
    otk_hard_loop_defaults(c, STEP_TICKS);
    c->learn_ns = 300000000u;    /* 0.3 s: about 150 replies on the fake's 2 ms cycle */
    c->cycle_guess_ns = 2000000.0;
    c->drain_silence_ns = 50000000u;
    c->timeout_ns = 30000000u;
    c->retries = 2u;
    c->sync_bin_ns = 20000000u;
}

/* Runs `steps` steps through the driver; returns the outcome. */
static OtkHardResult run(FakeBridge *f, OtkHardLoop *hl, const OtkHardLoopConfig *c, uint32_t steps,
                         OtkHardCapture *caps, size_t *n_caps) {
    char name[256];
    CHECK(fake_start(f, name, sizeof(name)) != NULL);
    char err[256] = "";
    const int rc = otk_hard_loop_open(hl, name, c, err, sizeof(err));
    CHECK(rc == 0);
    if (rc != 0) {
        printf("  open: %s\n", err);
        fake_stop(f);
        return OTK_HARD_ABORTED;
    }
    OtkDriver d;
    otk_driver_init(&d, OTK_DRIVER_HARD, 5000000u);
    otk_driver_attach_hard(&d, hl);
    size_t nc = 0;
    for (uint32_t k = 0; k < steps && !otk_driver_failed(&d); k++) {
        CHECK(otk_hard_loop_computed(hl) == k);
        const OtkBridgeSetpoint sp = sp_of(k);
        otk_hard_loop_set(hl, &sp);
        otk_driver_wait(&d);
        if (caps != NULL) {
            nc += otk_hard_loop_captures(hl, &caps[nc], 8u - nc);
        }
    }
    const OtkHardResult r = otk_hard_loop_stop(hl);
    fake_stop(f);
    if (getenv("OTK_TEST_VERBOSE") != NULL) {
        printf("  %s: %u underruns (fake %u), %u late, %u/%u applied wrong, %u seq, %u timeouts, min fill %u, send late max %llu us\n",
               otk_hard_result_name(r), hl->stats.underruns, f->underruns, hl->stats.late, hl->stats.applied_wrong,
               hl->stats.applied_checked, hl->stats.seq_errors, hl->stats.timeouts, hl->stats.min_fill,
               (unsigned long long)(hl->stats.send_late_max_ns / 1000u));
    }
    if (n_caps != NULL) {
        *n_caps = nc;
    }
    return r;
}

static void test_run_ok(void) {
    static FakeBridge f;
    fake_init(&f);
    f.drift = 6e-6;
    OtkHardLoopConfig c;
    config(&c);
    static OtkHardLoop hl;
    const OtkHardResult r = run(&f, &hl, &c, STEPS, NULL, NULL);
    CHECK(r == OTK_HARD_OK);
    CHECK(hl.stats.underruns == 0u && hl.stats.late == 0u && f.underruns == 0u);
    CHECK(hl.stats.applied_checked > 10u && hl.stats.applied_wrong == 0u);
    /* every step the bridge ran applied the setpoint computed for it */
    CHECK(f.stopped);
    CHECK(f.stop_step == STEPS - 1u); /* stopped once it began the last step the plant computed */
    unsigned wrong = 0;
    for (uint32_t k = 0; k <= f.stop_step; k++) {
        const OtkBridgeSetpoint want = sp_of(k);
        wrong += !f.applied_set[k] || memcmp(&f.applied[k], &want, sizeof(want)) != 0;
    }
    CHECK(wrong == 0u);
    /* the plant stayed about H ahead */
    CHECK(hl.stats.min_fill >= c.h - 3u && hl.stats.min_fill <= c.h + 2u);
    /* the fake's thread wakes up late now and then, and its late replies tilt the fit: on a quiet host the
     * period came out 1 µs off, with rarer tails past 2 µs. 20 µs (1 %) still tells a miscounted cycle
     * from the right one, and an error of that size moves a send by microseconds against a 600 µs margin */
    CHECK(hl.have_cycle && fabs(hl.cycle_p - 2000000.0) < 20000.0);
    CHECK(hl.stats.bound_max_ns > 0.0 && hl.stats.bound_max_ns < 2000000.0);
    CHECK(otk_hard_loop_rtt_pct(&hl, 50) > 0u && otk_hard_loop_rtt_pct(&hl, 99) < 5000000u);
    CHECK(hl.stats.seq_errors == 0u && hl.stats.timeouts == 0u);
    /* the budget file */
    FILE *fp = tmpfile();
    OtkHardBudget b;
    otk_hard_budget_none(&b);
    b.underruns = 0;
    b.late = 0;
    b.rtt_p99_ns = 5000000u;
    const OtkHardRunMeta meta = { "fake-pty", "none", "0123abcd", "deadbee", "test_hard_loop" };
    CHECK(otk_hard_budget_write(fp, &hl, r, &b, &meta, NULL));
    rewind(fp);
    char line[512];
    CHECK(fgets(line, sizeof(line), fp) != NULL);
    CHECK(strncmp(line, "# otk-hard-budget 1; result=ok; H=16; step_ns=5000000; margin_ns=1200000; tick_hz=27000000; "
                        "link=fake-pty; usb=none; bitstream_md5=0123abcd; commit=deadbee; source=test_hard_loop\n",
                  sizeof(line)) == 0);
    CHECK(fgets(line, sizeof(line), fp) != NULL && strcmp(line, "quantity,declared,measured,unit,within\n") == 0);
    bool saw_underruns = false, saw_lead = false, saw_rtt = false, saw_delay = false, saw_lag = false;
    while (fgets(line, sizeof(line), fp) != NULL) {
        saw_underruns |= strcmp(line, "underruns,0,0,steps,yes\n") == 0;
        saw_lead |= strcmp(line, "plant_lead_ns,80000000,80000000,ns,yes\n") == 0;
        saw_rtt |= strncmp(line, "rtt_p99_ns,5000000,", 19) == 0 && strstr(line, ",ns,yes\n") != NULL;
        /* declared: (16 + 1) x 5 ms + 2 x the 2 ms cycle guess + the 5 ms round trip */
        saw_delay |= strncmp(line, "dut_to_plant_delay_ns,94000000,", 31) == 0 && strstr(line, ",ns,yes\n") != NULL;
        saw_lag |= strcmp(line, "capture_lag_max_ns,94000000,,ns,\n") == 0; /* no capture in this run */
    }
    CHECK(saw_underruns && saw_lead && saw_rtt && saw_delay && saw_lag);
    fclose(fp);
    /* a value that would break the metadata line is refused */
    const OtkHardRunMeta bad = { "a;b", NULL, NULL, NULL, NULL };
    fp = tmpfile();
    CHECK(!otk_hard_budget_write(fp, &hl, r, &b, &bad, NULL));
    fclose(fp);
    otk_hard_loop_free(&hl);
}

static void test_underrun_is_degraded(void) {
    static FakeBridge f;
    fake_init(&f);
    f.drop_setpoint_at = 40u;
    OtkHardLoopConfig c;
    config(&c);
    static OtkHardLoop hl;
    CHECK(run(&f, &hl, &c, 80u, NULL, NULL) == OTK_HARD_DEGRADED);
    CHECK(hl.stats.underruns == 1u && f.underruns == 1u);
    /* the bridge held step 39's setpoint through step 40 */
    const OtkBridgeSetpoint held = sp_of(39u);
    CHECK(memcmp(&f.applied[40], &held, sizeof(held)) == 0);
    otk_hard_loop_free(&hl);
}

static void test_late_is_degraded(void) {
    static FakeBridge f;
    fake_init(&f);
    f.report_late_at = 30u;
    OtkHardLoopConfig c;
    config(&c);
    static OtkHardLoop hl;
    CHECK(run(&f, &hl, &c, 60u, NULL, NULL) == OTK_HARD_DEGRADED);
    CHECK(hl.stats.late == 1u && hl.stats.underruns == 0u);
    otk_hard_loop_free(&hl);
}

static void test_wrong_applied_is_degraded(void) {
    static FakeBridge f;
    fake_init(&f);
    f.wrong_applied_from = 20u;
    OtkHardLoopConfig c;
    config(&c);
    static OtkHardLoop hl;
    CHECK(run(&f, &hl, &c, 60u, NULL, NULL) == OTK_HARD_DEGRADED);
    CHECK(hl.stats.applied_wrong > 0u && hl.stats.underruns == 0u && hl.stats.late == 0u);
    otk_hard_loop_free(&hl);
}

static void test_silence_is_aborted(void) {
    static FakeBridge f;
    fake_init(&f);
    f.silent_from = 30u;
    OtkHardLoopConfig c;
    config(&c);
    static OtkHardLoop hl;
    CHECK(run(&f, &hl, &c, 400u, NULL, NULL) == OTK_HARD_ABORTED);
    CHECK(hl.aborted && hl.computed < 400u);
    CHECK(hl.link.timeouts == c.retries + 1u); /* one request, sent 1 + retries times */
    CHECK(!f.stopped);
    otk_hard_loop_free(&hl);
}

static void test_capture_has_its_step(void) {
    static FakeBridge f;
    fake_init(&f);
    f.capture_at = 25u;
    OtkHardLoopConfig c;
    config(&c);
    static OtkHardLoop hl;
    OtkHardCapture caps[8];
    size_t n = 0;
    CHECK(run(&f, &hl, &c, 60u, caps, &n) == OTK_HARD_OK);
    CHECK(n == 1u);
    if (n == 1u) {
        CHECK(caps[0].channel == 2u && caps[0].width == 5000u && caps[0].step == 25u);
        CHECK(caps[0].start_tick == f.t_start + 25u * STEP_TICKS + 1000u); /* extended to 64 bits exactly */
    }
    /* the plant was at least H steps on when it got the capture, and within the declared delay */
    const uint64_t bound = otk_hard_dut_to_plant_ns(c.h, 5000000u, 2000000u, otk_hard_loop_rtt_pct(&hl, 100.0));
    CHECK(hl.stats.capture_lag_max >= c.h && (uint64_t)hl.stats.capture_lag_max * 5000000u <= bound);
    otk_hard_loop_free(&hl);
}

static void test_dut_to_plant(void) {
    /* (H + 1) x step + 2 x cycle + round trip: H = 16, 1 ms steps, the BL616's 6 ms, 2.2 ms */
    CHECK(otk_hard_dut_to_plant_ns(16u, 1000000u, 6000000u, 2200000u) == 31200000u);
    CHECK(otk_hard_dut_to_plant_ns(0u, 10000000u, 0u, 0u) == 10000000u);
}

static void test_refuses_a_used_bridge(void) {
    static FakeBridge f;
    fake_init(&f);
    f.initial_state = OTK_BRIDGE_STOPPED;
    char name[256], err[256] = "";
    CHECK(fake_start(&f, name, sizeof(name)) != NULL);
    OtkHardLoopConfig c;
    config(&c);
    static OtkHardLoop hl;
    CHECK(otk_hard_loop_open(&hl, name, &c, err, sizeof(err)) == -1);
    CHECK(strstr(err, "not freshly programmed") != NULL);
    fake_stop(&f);
    otk_hard_loop_free(&hl);
}

static void test_refuses_missing_capabilities(void) {
    static FakeBridge f;
    fake_init(&f);
    f.n_analog = 2u;
    char name[256], err[256] = "";
    CHECK(fake_start(&f, name, sizeof(name)) != NULL);
    OtkHardLoopConfig c;
    config(&c);
    c.need_analog = 4u;
    static OtkHardLoop hl;
    CHECK(otk_hard_loop_open(&hl, name, &c, err, sizeof(err)) == -1);
    CHECK(strstr(err, "2 analog outputs") != NULL);
    fake_stop(&f);
    otk_hard_loop_free(&hl);
}

static void test_refuses_a_bad_h(void) {
    OtkHardLoopConfig c;
    config(&c);
    c.h = OTK_HARD_MAX_H + 1u;
    static OtkHardLoop hl;
    char err[128] = "";
    CHECK(otk_hard_loop_open(&hl, "/dev/null", &c, err, sizeof(err)) == -1 && strstr(err, "H must be") != NULL);
    otk_hard_loop_free(&hl);
}

static void test_aim(void) {
    OtkHardAim a = { 27000000u, 270000u, 1000000000u, 5000000000u, 900000000u, 5e-6 };
    /* step k begins at t_start + k x step_ticks: host time and back */
    for (uint32_t k = 0; k < 1000u; k += 37u) {
        const uint64_t h = otk_hard_aim_step_host_ns(&a, k);
        const double tick = otk_hard_aim_tick(&a, h);
        CHECK(fabs(tick - (1000000000.0 + (double)k * 270000.0)) < 0.1);
        CHECK(otk_hard_aim_step(&a, h + 1000u) == k);
        CHECK(k == 0u || otk_hard_aim_step(&a, h - 1000u) == k - 1u);
    }
    /* the drift is applied: a 10 ms step is 10 ms / (1 + 5 ppm) of host time */
    const double dt = (double)(otk_hard_aim_step_host_ns(&a, 101u) - otk_hard_aim_step_host_ns(&a, 1u));
    CHECK(fabs(dt - 1e9 / (1.0 + 5e-6)) < 2.0);
    CHECK(otk_hard_aim_step(&a, 0u) == 0u); /* before step 0 */
}

static void test_rt_mono(void) {
    OtkRtMono m;
    otk_rt_mono_init(&m);
    CHECK(otk_rt_mono_err_ns(&m) == UINT64_MAX);
    otk_rt_mono_add(&m, 1000000u, 400u, 1000200u); /* offset 999 700, width 200 */
    CHECK(m.offset_ns == 999700 && otk_rt_mono_err_ns(&m) == 100u);
    otk_rt_mono_add(&m, 2000050u, 1000400u, 2000150u); /* offset 999 700, moved by 0 */
    otk_rt_mono_add(&m, 3000000u, 2000000u, 3000020u); /* offset 1 000 010: NTP moved it 310 ns */
    CHECK(otk_rt_mono_err_ns(&m) == 100u + 310u);
    CHECK(otk_rt_mono_to_mono(&m, 5000010u) == 4000000u);
    CHECK(!otk_rt_mono_add(&m, 5000u, 1u, 4000u)); /* REALTIME went back: not a sample */
    CHECK(m.samples == 3u && m.rejected == 1u);
    /* the real clocks */
    OtkRtMono r;
    otk_rt_mono_init(&r);
    for (int i = 0; i < 100; i++) {
        otk_rt_mono_sample(&r);
    }
    CHECK(r.have && otk_rt_mono_err_ns(&r) < 1000000u);
}

/* A scripted clock: each read returns the next value, whichever clock it is. */
typedef struct {
    const uint64_t *v;
    size_t n, i;
} ScriptClock;

static uint64_t script_read(void *ctx, int id) {
    (void)id;
    ScriptClock *c = ctx;
    CHECK(c->i < c->n);
    return c->i < c->n ? c->v[c->i++] : 0u;
}

static void test_rt_mono_width(void) {
    /* a wide sample (19.3 us, like the one interrupted read of F-39) is dropped,
     * and the bound does not see it */
    OtkRtMono m;
    otk_rt_mono_init(&m);
    CHECK(otk_rt_mono_add(&m, 1000000u, 400u, 1000200u)); /* offset 999 700, half width 100 */
    CHECK(!otk_rt_mono_add(&m, 2000000u, 1000000u, 2019300u));
    CHECK(m.samples == 1u && m.rejected == 1u && otk_rt_mono_err_ns(&m) == 100u);
    /* the limit itself is accepted */
    CHECK(otk_rt_mono_add(&m, 3000000u, 3000000u + OTK_RT_MONO_MAX_WIDTH_NS / 2u - 999700u,
                          3000000u + OTK_RT_MONO_MAX_WIDTH_NS));
    CHECK(!otk_rt_mono_add(&m, 4000000u, 2999700u, 4000000u + OTK_RT_MONO_MAX_WIDTH_NS + 1u));
    CHECK(m.samples == 2u && m.rejected == 2u && otk_rt_mono_err_ns(&m) == OTK_RT_MONO_MAX_WIDTH_NS / 2u);

    /* one wide read is read again: the second, narrow one is the sample */
    const uint64_t retry[] = {1000000u, 400u, 1019300u, 2000000u, 1000300u, 2000200u};
    ScriptClock c = {retry, 6u, 0u};
    otk_rt_mono_init(&m);
    otk_rt_mono_sample_from(&m, script_read, &c);
    CHECK(c.i == 6u && m.samples == 1u && m.retries == 1u && m.rejected == 0u);
    CHECK(m.offset_ns == 999800 && otk_rt_mono_err_ns(&m) == 100u);

    /* three wide reads in a row: dropped and counted once */
    const uint64_t wide[] = {0u, 0u, 5000u, 10000u, 0u, 15000u, 20000u, 0u, 25000u};
    c = (ScriptClock){wide, 9u, 0u};
    otk_rt_mono_sample_from(&m, script_read, &c);
    CHECK(c.i == 9u && m.samples == 1u && m.retries == 3u && m.rejected == 1u && otk_rt_mono_err_ns(&m) == 100u);

    /* a step of REALTIME still shows in the bound: between samples ... */
    const uint64_t step[] = {3000000u, 2000000u, 3000200u, 4005000u, 3000000u, 4005200u};
    c = (ScriptClock){step, 6u, 0u};
    otk_rt_mono_sample_from(&m, script_read, &c); /* offset 1 000 100 */
    otk_rt_mono_sample_from(&m, script_read, &c); /* offset 1 005 100: REALTIME stepped 5 us */
    CHECK(m.samples == 3u && m.rejected == 1u);
    CHECK(otk_rt_mono_err_ns(&m) == 100u + (1005100u - 999800u));
    /* ... and inside one: that read is too wide, the next one has the new offset */
    const uint64_t inside[] = {5000000u, 4000000u, 1005000200u, 1005000000u, 4000100u, 1005000200u};
    c = (ScriptClock){inside, 6u, 0u};
    otk_rt_mono_sample_from(&m, script_read, &c); /* offset 1 001 000 000 */
    CHECK(m.samples == 4u && m.retries == 4u && m.rejected == 1u);
    CHECK(otk_rt_mono_err_ns(&m) == 100u + (1001000000u - 999800u));
}

static void test_usb_path(void) {
    char out[64];
    CHECK(!otk_hard_usb_path("/dev/null", out, sizeof(out)) && strcmp(out, "none") == 0);
    CHECK(!otk_hard_usb_path("/nonexistent", out, sizeof(out)) && strcmp(out, "none") == 0);
}

static void test_driver_names(void) {
    OtkDriverKind k;
    CHECK(otk_driver_from_name("hard", &k) && k == OTK_DRIVER_HARD);
    CHECK(strcmp(otk_driver_name(OTK_DRIVER_HARD), "hard") == 0);
    OtkDriver d;
    otk_driver_init(&d, OTK_DRIVER_HARD, 1000000u);
    otk_driver_wait(&d); /* no loop attached */
    CHECK(otk_driver_failed(&d));
    CHECK(strcmp(otk_hard_result_name(OTK_HARD_DEGRADED), "degraded") == 0);
}

int main(void) {
    RUN_TEST(test_dut_to_plant);
    RUN_TEST(test_aim);
    RUN_TEST(test_rt_mono);
    RUN_TEST(test_rt_mono_width);
    RUN_TEST(test_usb_path);
    RUN_TEST(test_driver_names);
    RUN_TEST(test_refuses_a_bad_h);
    RUN_TEST(test_refuses_a_used_bridge);
    RUN_TEST(test_refuses_missing_capabilities);
    RUN_TEST(test_run_ok);
    RUN_TEST(test_underrun_is_degraded);
    RUN_TEST(test_late_is_degraded);
    RUN_TEST(test_wrong_applied_is_degraded);
    RUN_TEST(test_silence_is_aborted);
    RUN_TEST(test_capture_has_its_step);
    return check_report();
}
