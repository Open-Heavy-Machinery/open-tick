/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_player.c
 * @brief Scenario player: when events apply, in which order, and `end`.
 */

#include <string.h>

#include "check.h"
#include <opentick/player.h>

enum { SIG_A, SIG_B, SIG_COUNT };
static const char *const NAMES[SIG_COUNT] = { [SIG_A] = "a", [SIG_B] = "b" };
static const OtkSignalTable TABLE = { NAMES, SIG_COUNT };

static OtkScenario scn;

typedef struct {
    double a;
    double b;
    unsigned calls;
    OtkSignalId seen[16];
} Machine;

static void apply(void *ctx, OtkSignalId signal, double value) {
    Machine *m = ctx;
    if (m->calls < 16U) {
        m->seen[m->calls] = signal;
    }
    m->calls++;
    if (signal == SIG_A) {
        m->a = value;
    } else if (signal == SIG_B) {
        m->b = value;
    }
}

static void parse(const char *text) {
    char err[128] = "";
    const bool ok = otk_scenario_parse(text, &TABLE, &scn, err, sizeof(err));
    CHECK(ok);
    if (!ok) {
        fprintf(stderr, "  %s\n", err);
    }
}

static const OtkNs STEP = 10U * OTK_NS_PER_MS;

static void test_events_apply_at_the_start_of_the_next_step(void) {
    /* Same case as open-crank v0.1's test_events_apply_in_their_step. */
    parse("time_s,signal,value\n0.015,a,1000\n0.015,a,1200\n");
    Machine m = { 0 };
    OtkPlayer p;
    otk_player_init(&p, &scn, apply, &m);

    otk_player_advance(&p, 0U * STEP);
    otk_player_advance(&p, 1U * STEP);
    CHECK(m.a == 0.0 && m.calls == 0U);
    otk_player_advance(&p, 2U * STEP); /* step starting at 20 ms: the 15 ms events are due */
    CHECK(m.a == 1200.0);              /* same time: file order */
    CHECK(m.calls == 2U);
    CHECK(!otk_player_done(&p, 2U * STEP));
}

static void test_event_on_a_step_boundary_applies_in_that_step(void) {
    parse("time_s,signal,value\n0.010,a,1\n");
    Machine m = { 0 };
    OtkPlayer p;
    otk_player_init(&p, &scn, apply, &m);
    otk_player_advance(&p, STEP - 1U);
    CHECK(m.a == 0.0);
    otk_player_advance(&p, STEP);
    CHECK(m.a == 1.0);
}

static void test_each_event_applies_once_and_in_order(void) {
    parse("time_s,signal,value\n0,a,1\n0.001,b,2\n0.002,a,3\n0.050,b,4\n");
    Machine m = { 0 };
    OtkPlayer p;
    otk_player_init(&p, &scn, apply, &m);
    otk_player_advance(&p, 100U * OTK_NS_PER_MS); /* one long jump */
    otk_player_advance(&p, 100U * OTK_NS_PER_MS); /* nothing left to apply */
    otk_player_advance(&p, 50U * OTK_NS_PER_MS);  /* time going back applies nothing again */
    CHECK(m.calls == 4U);
    CHECK(m.seen[0] == SIG_A && m.seen[1] == SIG_B && m.seen[2] == SIG_A && m.seen[3] == SIG_B);
    CHECK(m.a == 3.0 && m.b == 4.0);
}

static void test_end_is_not_handed_to_the_application(void) {
    parse("time_s,signal,value\n0,a,1\n0.020,end,0\n");
    Machine m = { 0 };
    OtkPlayer p;
    otk_player_init(&p, &scn, apply, &m);
    otk_player_advance(&p, 0U);
    CHECK(!otk_player_done(&p, 0U));
    CHECK(!otk_player_done(&p, 20U * OTK_NS_PER_MS - 1U));
    otk_player_advance(&p, 20U * OTK_NS_PER_MS);
    CHECK(m.calls == 1U);
    CHECK(otk_player_done(&p, 20U * OTK_NS_PER_MS));
    CHECK(otk_player_done(&p, 30U * OTK_NS_PER_MS));
}

static void test_no_end_never_done(void) {
    parse("time_s,signal,value\n0,a,1\n");
    Machine m = { 0 };
    OtkPlayer p;
    otk_player_init(&p, &scn, apply, &m);
    otk_player_advance(&p, UINT64_MAX);
    CHECK(!otk_player_done(&p, UINT64_MAX));
}

static void test_no_scenario(void) {
    Machine m = { 0 };
    OtkPlayer p;
    otk_player_init(&p, NULL, apply, &m);
    otk_player_advance(&p, 1000U * OTK_NS_PER_MS);
    CHECK(m.calls == 0U);
    CHECK(!otk_player_done(&p, UINT64_MAX));
}

int main(void) {
    RUN_TEST(test_events_apply_at_the_start_of_the_next_step);
    RUN_TEST(test_event_on_a_step_boundary_applies_in_that_step);
    RUN_TEST(test_each_event_applies_once_and_in_order);
    RUN_TEST(test_end_is_not_handed_to_the_application);
    RUN_TEST(test_no_end_never_done);
    RUN_TEST(test_no_scenario);
    return check_report();
}
