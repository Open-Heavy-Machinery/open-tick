/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_driver.c
 * @brief Pacer and drivers on the host's wall clock.
 *
 * Wall-clock tests only check bounds that hold on a loaded CI machine:
 * absolute deadlines mean a paced loop can never finish early, however
 * late it runs; an upper bound would make the test flaky, so there is none.
 */

#define _POSIX_C_SOURCE 200809L

#include <string.h>
#include <time.h>

#include "check.h"
#include <opentick/host/driver.h>
#include <opentick/host/pacer.h>

static OtkNs mono_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (OtkNs)ts.tv_sec * OTK_NS_PER_S + (OtkNs)ts.tv_nsec;
}

static void sleep_ns(OtkNs ns) {
    struct timespec ts = { (time_t)(ns / OTK_NS_PER_S), (long)(ns % OTK_NS_PER_S) };
    while (nanosleep(&ts, &ts) != 0) {
    }
}

static void test_paced_loop_never_finishes_early(void) {
    const OtkNs period = 2U * OTK_NS_PER_MS;
    const OtkNs t0 = mono_ns();
    OtkPacer p;
    otk_pacer_init(&p, period);
    for (int i = 0; i < 10; i++) {
        otk_pacer_wait(&p);
    }
    const OtkNs elapsed = mono_ns() - t0;
    CHECK(elapsed >= 10U * period);
    if (elapsed < 10U * period) {
        fprintf(stderr, "  10 periods of 2 ms took %llu ns\n", (unsigned long long)elapsed);
    }
}

static void test_overruns_are_counted_and_measured(void) {
    OtkPacer p;
    otk_pacer_init(&p, OTK_NS_PER_MS);
    sleep_ns(5U * OTK_NS_PER_MS); /* a step that takes 5 ms of a 1 ms period */
    otk_pacer_wait(&p);
    CHECK(p.overruns == 1U);
    CHECK(p.last_late >= 3U * OTK_NS_PER_MS);
    CHECK(p.max_late >= p.last_late);

    /* The schedule restarts from now instead of bursting to catch up. */
    const OtkNs t0 = mono_ns();
    otk_pacer_wait(&p);
    CHECK(mono_ns() - t0 >= OTK_NS_PER_MS / 2U);
}

static void test_zero_period_is_clamped(void) {
    OtkPacer p;
    otk_pacer_init(&p, 0U);
    CHECK(p.period == 1U);
}

static void test_free_run_does_not_sleep(void) {
    OtkDriver d;
    otk_driver_init(&d, OTK_DRIVER_FREE_RUN, 10U * OTK_NS_PER_MS);
    const OtkNs t0 = mono_ns();
    for (int i = 0; i < 100000; i++) {
        otk_driver_wait(&d); /* 1000 s of simulated time */
    }
    CHECK(d.steps == 100000U);
    CHECK(mono_ns() - t0 < OTK_NS_PER_S); /* generous: it should take microseconds */
}

static void test_paced_driver_paces(void) {
    OtkDriver d;
    otk_driver_init(&d, OTK_DRIVER_PACED, OTK_NS_PER_MS);
    const OtkNs t0 = mono_ns();
    for (int i = 0; i < 5; i++) {
        otk_driver_wait(&d);
    }
    CHECK(d.steps == 5U);
    CHECK(mono_ns() - t0 >= 5U * OTK_NS_PER_MS);
}

static void test_names(void) {
    OtkDriverKind k;
    CHECK(otk_driver_from_name("free-run", &k) && k == OTK_DRIVER_FREE_RUN);
    CHECK(otk_driver_from_name("paced", &k) && k == OTK_DRIVER_PACED);
    CHECK(!otk_driver_from_name("hard", &k));
    CHECK(strcmp(otk_driver_name(OTK_DRIVER_PACED), "paced") == 0);
    CHECK(strcmp(otk_driver_name(OTK_DRIVER_FREE_RUN), "free-run") == 0);
}

int main(void) {
    RUN_TEST(test_paced_loop_never_finishes_early);
    RUN_TEST(test_overruns_are_counted_and_measured);
    RUN_TEST(test_zero_period_is_clamped);
    RUN_TEST(test_free_run_does_not_sleep);
    RUN_TEST(test_paced_driver_paces);
    RUN_TEST(test_names);
    return check_report();
}
