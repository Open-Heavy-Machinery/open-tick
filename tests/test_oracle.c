/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_oracle.c
 * @brief Acceptance oracle: criteria format, windows, tolerance, staleness,
 *        implication and the reaction time it reports. A synthetic table of
 *        observables and hand-made frames; no bus.
 */

#include <string.h>

#include "check.h"
#include <opentick/oracle.h>

/* ------------------------------------------------------------ synthetic table */

/* Frame id 1: a = data[0]. Frame id 2: b = data[0]. Frame id 3: b is a gap. */
enum { OBS_A = 0, OBS_B, OBS_COUNT };

static const OtkOracleObservable OBSERVABLES[OBS_COUNT] = {
    [OBS_A] = { "a", 50U * OTK_NS_PER_MS, { false, 0.0 } },
    [OBS_B] = { "b", 50U * OTK_NS_PER_MS, { true, 0.0 } },
};

static size_t decode(void *ctx, const OtkCanFrame *f, OtkOracleUpdate *out, size_t cap) {
    (void)ctx;
    if (cap < 1U) {
        return 0U;
    }
    switch (f->id) {
    case 1U:
        out[0] = (OtkOracleUpdate){ OBS_A, { false, (double)f->data[0] } };
        return 1U;
    case 2U:
        out[0] = (OtkOracleUpdate){ OBS_B, { false, (double)f->data[0] } };
        return 1U;
    case 3U:
        out[0] = (OtkOracleUpdate){ OBS_B, { true, 0.0 } };
        return 1U;
    default:
        return 0U;
    }
}

static const OtkOracleTable TABLE = { OBSERVABLES, OBS_COUNT, decode, NULL };

static const char *const SIGNAL_NAMES[] = { "go", "stop" };
static const OtkSignalTable SIGNALS = { SIGNAL_NAMES, 2U };

/* go=1 at 0.1 s, stop=1 at 0.3 s, end at 1.0 s. */
static const char *const SCENARIO_TEXT =
    "time_s,signal,value\n"
    "0.0,go,0\n"
    "0.1,go,1\n"
    "0.3,stop,1\n"
    "1.0,end,0\n";

#define STEP_MS 10U
#define END_MS 1000U
#define MS(x) ((int64_t)(x) * 1000000)

static OtkScenario scn; /* large: keep off the stack */
static OtkOracle oracle;

static bool load(const char *criteria) {
    char err[200] = "";
    const bool ok =
        otk_oracle_parse(&oracle, criteria, &TABLE, &scn, &SIGNALS, otk_ns_from_ms(STEP_MS), err, sizeof(err));
    if (!ok) {
        fprintf(stderr, "  unexpected error: %s\n", err);
    }
    return ok;
}

static void send(uint32_t id, uint8_t value, uint32_t t_ms) {
    OtkCanFrame f;
    memset(&f, 0, sizeof(f));
    f.id = id;
    f.len = 1U;
    f.data[0] = value;
    otk_oracle_frame(&oracle, &f, otk_ns_from_ms(t_ms));
}

/* Runs 0..END_MS: at each step, a = a_at(t) is sent (unless negative), then the step is evaluated. */
static void run(int (*a_at)(uint32_t t_ms)) {
    for (uint32_t t = 0U; t <= END_MS; t += STEP_MS) {
        const int a = a_at(t);
        if (a >= 0) {
            send(1U, (uint8_t)a, t);
        }
        otk_oracle_step(&oracle, otk_ns_from_ms(t));
    }
}

/* The report as text. */
static const char *report(void) {
    static char buf[4096];
    memset(buf, 0, sizeof(buf));
    FILE *fp = tmpfile();
    if (fp == NULL) {
        return "";
    }
    (void)otk_oracle_report(&oracle, fp);
    rewind(fp);
    const size_t n = fread(buf, 1U, sizeof(buf) - 1U, fp);
    buf[n] = '\0';
    fclose(fp);
    return buf;
}

static void check_report_has(const char *fragment) {
    const char *r = report();
    CHECK(strstr(r, fragment) != NULL);
    if (strstr(r, fragment) == NULL) {
        fprintf(stderr, "  report:\n%s  wanted '%s'\n", r, fragment);
    }
}

/* ------------------------------------------------------------ format */

static void expect_error(const char *criteria, const char *fragment) {
    char err[200] = "";
    CHECK(!otk_oracle_parse(&oracle, criteria, &TABLE, &scn, &SIGNALS, otk_ns_from_ms(STEP_MS), err, sizeof(err)));
    CHECK(otk_oracle_count(&oracle) == 0U);
    CHECK(strstr(err, fragment) != NULL);
    if (strstr(err, fragment) == NULL) {
        fprintf(stderr, "  got: '%s', wanted '%s'\n", err, fragment);
    }
}

#define H "id,from,until,tolerance_ms,condition,text\n"

static void test_parses_valid_text(void) {
    CHECK(load("# a comment\n"
               "\n"
               H
               "  # indented comment\n"
               "X1,begin,end,0,a == 1,text, with commas, kept\r\n"
               "X2,0.05,go=1+200ms,20,a >= 1 && b < 3 -> a != 2 && b <= 9,second\n"));
    CHECK(otk_oracle_count(&oracle) == 2U);
    const OtkOracleCriterion *c = &oracle.criteria[0];
    CHECK(strcmp(c->id, "X1") == 0);
    CHECK(strcmp(c->text, "text, with commas, kept") == 0);
    CHECK(c->from_ns == MS(0) && c->until_ns == MS(1000));
    CHECK(c->n_terms == 1U && c->n_antecedent == 0U);
    c = &oracle.criteria[1];
    CHECK(c->from_ns == MS(50) && c->until_ns == MS(300) && c->tolerance_ns == MS(20));
    CHECK(c->n_terms == 4U && c->n_antecedent == 2U);
}

static void test_rejects_bad_text(void) {
    expect_error("id,from,until,tol,condition,text\nX,begin,end,0,a == 1,t\n", "line 1: expected the header");
    expect_error("# comment\n\nid,from,until\n", "line 3: expected the header");
    expect_error(H "X,begin,end,0,zz == 1,t\n", "line 2: unknown observable 'zz'");
    expect_error(H "# c\nX,begin,end,0,a == 1 && zz < 2,t\n", "line 3: unknown observable 'zz'");
    expect_error(H "X,go=5,end,0,a == 1,t\n", "line 2: no event in the scenario sets 'go' to 5");
    expect_error(H "X,begin,nope=1,0,a == 1,t\n", "line 2: unknown signal 'nope'");
    expect_error(H "X,begin,end,0,a = 1,t\n", "line 2: bad operator in term 'a = 1'");
    expect_error(H "X,begin,end,0,a => 1,t\n", "line 2: bad operator");
    expect_error(H "X,begin,end,0,a <> 1,t\n", "line 2: bad operator");
    expect_error(H "X,begin,end,0,a !== 1,t\n", "line 2: bad operator");
    expect_error(H "X,begin,end,0,a 1,t\n", "line 2: no operator");
    expect_error(H "X,begin,end,0,a == x,t\n", "line 2: bad number 'x'");
    expect_error(H "X,begin,end,0,a == 1\n", "line 2: expected six fields");
    expect_error(H "X,begin,end,0,a == 1,t\nX,begin,end,0,a == 1,t\n", "line 3: duplicate id 'X'");
    expect_error(H "X,begin,end,-1,a == 1,t\n", "line 2: bad tolerance_ms");
    expect_error(H "X,begin,end,0,a == 1 -> a == 1 -> a == 1,t\n", "line 2: more than one '->'");
    expect_error(H "X,begin,end,0,a == 1 &&,t\n", "line 2: empty term");
    expect_error(H "X,begin,end,0,a==1&&a==1&&a==1&&a==1&&a==1&&a==1&&a==1&&a==1&&a==1,t\n",
                 "line 2: more than 8 terms");
    expect_error(H "X,0.5,0.2,0,a == 1,t\n", "line 2: empty window");
    expect_error(H "X,begin-10ms,end,0,a == 1,t\n", "line 2: from 'begin-10ms' is before t = 0");
    expect_error(H "X,begin,end+10ms,0,a == 1,t\n", "line 2: until 1.010 s is after the scenario's end");
    expect_error(H "X,begin,soon,0,a == 1,t\n", "line 2: bad reference 'soon' in until");
    expect_error(H "X,begin,end,0,a == 1,\n", "line 2: empty text");
    expect_error(H, "no criteria");
    expect_error("# nothing\n", "no header");
}

/* ------------------------------------------------------------ windows */

static int a_one(uint32_t t) {
    (void)t;
    return 1;
}
static int a_zero_at_90_and_310(uint32_t t) {
    return (t == 90U || t == 310U) ? 0 : 1;
}
static int a_zero_at_300(uint32_t t) {
    return (t == 300U) ? 0 : 1;
}
static int a_zero_at_100(uint32_t t) {
    return (t == 100U) ? 0 : 1;
}
static int a_zero_at_110_and_290(uint32_t t) {
    return (t == 110U || t == 290U) ? 0 : 1;
}
static int a_zero_at_120(uint32_t t) {
    return (t == 120U) ? 0 : 1;
}
static int a_zero_at_280(uint32_t t) {
    return (t == 280U) ? 0 : 1;
}

static void test_windows_are_inclusive(void) {
    const char *crit = H "W,go=1,stop=1,0,a == 1,go to stop\n";
    CHECK(load(crit));
    CHECK(oracle.criteria[0].from_ns == MS(100) && oracle.criteria[0].until_ns == MS(300));
    run(a_one);
    CHECK(otk_oracle_failures(&oracle) == 0U);

    CHECK(load(crit));
    run(a_zero_at_90_and_310); /* just outside: not graded */
    CHECK(otk_oracle_failures(&oracle) == 0U);

    CHECK(load(crit));
    run(a_zero_at_100); /* first step of the window */
    CHECK(otk_oracle_failed(&oracle, "W"));

    CHECK(load(crit));
    run(a_zero_at_300); /* last step of the window */
    CHECK(otk_oracle_failed(&oracle, "W"));
    check_report_has("FAIL  W  go to stop  [0.10 s .. 0.30 s]  first violation at 0.30 s: a = 0");
}

static void test_window_offsets(void) {
    const char *crit = H "W,go=1+20ms,stop=1-20ms,0,a == 1,offsets\n";
    CHECK(load(crit));
    CHECK(oracle.criteria[0].from_ns == MS(120) && oracle.criteria[0].until_ns == MS(280));

    run(a_zero_at_110_and_290);
    CHECK(otk_oracle_failures(&oracle) == 0U);

    CHECK(load(crit));
    run(a_zero_at_120);
    CHECK(otk_oracle_failed(&oracle, "W"));

    CHECK(load(crit));
    run(a_zero_at_280);
    CHECK(otk_oracle_failed(&oracle, "W"));

    CHECK(load(H "T,0.25+5ms,end-100ms,0,a == 1,seconds\n"));
    CHECK(oracle.criteria[0].from_ns == MS(255) && oracle.criteria[0].until_ns == MS(900));
}

/* ------------------------------------------------------------ tolerance */

static int a_false_one_step(uint32_t t) {
    return (t == 500U) ? 0 : 1;
}
static int a_false_two_steps(uint32_t t) {
    return (t == 500U || t == 510U) ? 0 : 1;
}
static int a_false_three_steps(uint32_t t) {
    return (t >= 500U && t <= 520U) ? 0 : 1;
}
static int a_false_twice_two_steps(uint32_t t) {
    return (t == 500U || t == 510U || t == 600U || t == 610U) ? 0 : 1;
}

static void test_tolerance(void) {
    const char *zero = H "Z,begin,end,0,a == 1,zero\n";
    const char *twenty = H "T,begin,end,20,a == 1,twenty\n";

    CHECK(load(zero));
    run(a_false_one_step);
    CHECK(otk_oracle_failed(&oracle, "Z"));

    CHECK(load(twenty));
    run(a_false_one_step);
    CHECK(!otk_oracle_failed(&oracle, "T"));

    CHECK(load(twenty));
    run(a_false_two_steps);
    CHECK(!otk_oracle_failed(&oracle, "T"));

    CHECK(load(twenty));
    run(a_false_twice_two_steps); /* runs do not add up */
    CHECK(!otk_oracle_failed(&oracle, "T"));

    CHECK(load(twenty));
    run(a_false_three_steps);
    CHECK(otk_oracle_failed(&oracle, "T"));
    check_report_has("first violation at 0.50 s: a = 0"); /* the start of the run */
}

/* ------------------------------------------------------------ staleness and gaps */

static int a_only_at_0(uint32_t t) {
    return (t == 0U) ? 1 : -1;
}

static void test_stale_values_fall_back_to_the_default(void) {
    /* a = 1 is sent once, at t = 0; stale is 50 ms, the default is 0. */
    CHECK(load(H "S,begin,0.05,0,a == 1,fresh for 50 ms\n"));
    run(a_only_at_0);
    CHECK(!otk_oracle_failed(&oracle, "S"));

    CHECK(load(H "S,begin,0.06,0,a == 1,stale after 50 ms\n"));
    run(a_only_at_0);
    CHECK(otk_oracle_failed(&oracle, "S"));
    check_report_has("first violation at 0.06 s: a = 0");

    /* Before the first update, the default holds. */
    CHECK(load(H "D,begin,0.0,0,a == 0,default before any frame\n"));
    otk_oracle_step(&oracle, otk_ns_from_ms(0U));
    CHECK(!otk_oracle_failed(&oracle, "D"));
}

static void test_a_gap_makes_a_term_false(void) {
    /* b's default is a gap: every term on it is false, != included. */
    CHECK(load(H "G1,begin,0.0,0,b != 5,gap\n"
                 "G2,begin,0.0,0,b < 100,gap\n"
                 "G3,begin,0.0,0,a == 0,no gap\n"));
    otk_oracle_step(&oracle, otk_ns_from_ms(0U));
    CHECK(otk_oracle_failed(&oracle, "G1"));
    CHECK(otk_oracle_failed(&oracle, "G2"));
    CHECK(!otk_oracle_failed(&oracle, "G3"));
    check_report_has("FAIL  G1  gap  [0.00 s .. 0.00 s]  first violation at 0.00 s: b = gap");

    /* A decoded gap counts the same, and a stale gap falls back to the default. */
    CHECK(load(H "G,begin,0.02,0,b == 7,seven\n"));
    send(2U, 7U, 0U);
    otk_oracle_step(&oracle, otk_ns_from_ms(0U));
    send(3U, 0U, 10U);
    otk_oracle_step(&oracle, otk_ns_from_ms(10U));
    otk_oracle_step(&oracle, otk_ns_from_ms(20U));
    CHECK(otk_oracle_failed(&oracle, "G"));
    check_report_has("first violation at 0.01 s: b = gap");
}

/* ------------------------------------------------------------ implication */

static void test_implication(void) {
    const char *crit = H "I,begin,0.04,0,a == 1 -> b >= 10,b at least 10 while a is 1\n";

    CHECK(load(crit));
    send(1U, 0U, 0U); /* a = 0: true whatever b is (here a gap) */
    otk_oracle_step(&oracle, otk_ns_from_ms(0U));
    send(1U, 1U, 10U);
    send(2U, 20U, 10U); /* a = 1, b = 20 */
    otk_oracle_step(&oracle, otk_ns_from_ms(10U));
    send(2U, 10U, 20U);
    otk_oracle_step(&oracle, otk_ns_from_ms(20U));
    send(1U, 0U, 30U);
    send(2U, 3U, 30U);  /* a = 0 again: b may be anything */
    otk_oracle_step(&oracle, otk_ns_from_ms(30U));
    otk_oracle_step(&oracle, otk_ns_from_ms(40U));
    CHECK(!otk_oracle_failed(&oracle, "I"));

    CHECK(load(crit));
    send(1U, 1U, 0U);
    send(2U, 5U, 0U); /* a = 1, b = 5 */
    for (uint32_t t = 0U; t <= 40U; t += STEP_MS) {
        otk_oracle_step(&oracle, otk_ns_from_ms(t));
    }
    CHECK(otk_oracle_failed(&oracle, "I"));
    check_report_has("first violation at 0.00 s: a = 1, b = 5");

    /* a = 1 with b a gap: the consequent is false. */
    CHECK(load(crit));
    send(1U, 1U, 0U);
    otk_oracle_step(&oracle, otk_ns_from_ms(0U));
    CHECK(otk_oracle_failed(&oracle, "I"));
}

/* ------------------------------------------------------------ reaction time */

static int a_from_130(uint32_t t) {
    return (t >= 130U) ? 1 : 0;
}
static int a_flickers_then_from_170(uint32_t t) {
    return ((t >= 110U && t < 150U) || t >= 170U) ? 1 : 0;
}
static int a_true_before_event(uint32_t t) {
    (void)t;
    return 1;
}
static int a_lost_at_200(uint32_t t) {
    return (t >= 130U && t != 200U) ? 1 : 0;
}

static void test_reaction_time(void) {
    const char *crit = H "R,go=1+100ms,end,0,a == 1,a within 100 ms of go\n";

    CHECK(load(crit));
    run(a_from_130);
    CHECK(!otk_oracle_failed(&oracle, "R"));
    check_report_has("PASS  R  a within 100 ms of go  [0.20 s .. 1.00 s]  reaction: 30 ms after go=1\n");

    /* Only the last uninterrupted run up to `from` counts. */
    CHECK(load(crit));
    run(a_flickers_then_from_170);
    check_report_has("reaction: 70 ms after go=1");

    /* Already true at the event: 0 ms. */
    CHECK(load(crit));
    run(a_true_before_event);
    check_report_has("reaction: 0 ms after go=1");

    /* Not holding at `from`: the criterion fails at its first step and says so. */
    CHECK(load(crit));
    run(a_lost_at_200);
    CHECK(otk_oracle_failed(&oracle, "R"));
    check_report_has("reaction: not holding at 0.20 s  first violation at 0.20 s: a = 0");

    /* No reaction time unless `from` is an event plus an offset. */
    CHECK(load(H "N,0.2,end,0,a == 1,plain time\n"));
    run(a_from_130);
    CHECK(strstr(report(), "reaction") == NULL);
}

static void test_window_not_covered_fails(void) {
    CHECK(load(H "C,begin,end,0,a == 1,whole run\n"));
    for (uint32_t t = 0U; t <= 500U; t += STEP_MS) {
        send(1U, 1U, t);
        otk_oracle_step(&oracle, otk_ns_from_ms(t));
    }
    CHECK(otk_oracle_failed(&oracle, "C"));
    check_report_has("not evaluated to the end: the last step was at 0.50 s");
}

/* An event that does not fall on a whole millisecond: the reaction is measured from its exact time. */
static OtkScenario scn_fraction; /* large: keep off the stack */

static int a_from_110(uint32_t t) {
    return (t >= 110U) ? 1 : 0;
}

static void test_reaction_fraction(void) {
    static const char *const names[] = { "go" };
    static const OtkSignalTable signals = { names, 1U };
    char err[200] = "";
    CHECK(otk_scenario_parse("time_s,signal,value\n0.1005,go,1\n1.0,end,0\n", &signals, &scn_fraction, err,
                             sizeof(err)));
    CHECK(otk_oracle_parse(&oracle, H "R,go=1+50ms,end,0,a == 1,t\n", &TABLE, &scn_fraction, &signals,
                           otk_ns_from_ms(STEP_MS), err, sizeof(err)));
    run(a_from_110);
    CHECK(!otk_oracle_failed(&oracle, "R"));
    check_report_has("reaction: 9.500000 ms after go=1");
}

int main(void) {
    char err[200] = "";
    if (!otk_scenario_parse(SCENARIO_TEXT, &SIGNALS, &scn, err, sizeof(err))) {
        fprintf(stderr, "test scenario: %s\n", err);
        return 1;
    }
    RUN_TEST(test_parses_valid_text);
    RUN_TEST(test_rejects_bad_text);
    RUN_TEST(test_windows_are_inclusive);
    RUN_TEST(test_window_offsets);
    RUN_TEST(test_tolerance);
    RUN_TEST(test_stale_values_fall_back_to_the_default);
    RUN_TEST(test_a_gap_makes_a_term_false);
    RUN_TEST(test_implication);
    RUN_TEST(test_reaction_time);
    RUN_TEST(test_window_not_covered_fails);
    RUN_TEST(test_reaction_fraction);
    return check_report();
}
