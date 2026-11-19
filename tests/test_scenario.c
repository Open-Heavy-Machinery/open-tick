/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_scenario.c
 * @brief Scenario CSV parser.
 *
 * The cases from open-crank v0.1's test_scenario.c are kept, adapted to a
 * registered signal table and nanosecond times, so the extracted parser
 * accepts and rejects exactly what the original did.
 */

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include <opentick/scenario.h>

enum { SIG_KEY_ON, SIG_START, SIG_TARGET_RPM, SIG_HOIST, SIG_OVERRUN, SIG_COUNT };
static const char *const NAMES[SIG_COUNT] = {
    [SIG_KEY_ON] = "key_on",
    [SIG_START] = "start",
    [SIG_TARGET_RPM] = "target_rpm",
    [SIG_HOIST] = "hoist_demand_pct",
    [SIG_OVERRUN] = "fault.overrun_torque_nm",
};
static const OtkSignalTable TABLE = { NAMES, SIG_COUNT };

static OtkScenario scn; /* large: keep off the stack */

static void test_parses_valid_text(void) {
    const char *text =
        "# a comment\n"
        "\n"
        "time_s,signal,value\r\n"
        "0.0,key_on,1\n"
        "  0.5 , start , 1  \n"
        "3,start,0\n"
        "8.25,hoist_demand_pct,85.5\n"
        "8.25,fault.overrun_torque_nm,-12.5\n"
        "20,end,0\n";
    char err[128] = "";
    CHECK(otk_scenario_parse(text, &TABLE, &scn, err, sizeof(err)));
    CHECK(scn.n == 6U);
    CHECK(scn.events[1].signal == SIG_START && scn.events[1].t == 500000000U && scn.events[1].value == 1.0);
    CHECK(scn.events[3].t == 8250000000U && scn.events[3].value == 85.5);
    CHECK(scn.events[4].signal == SIG_OVERRUN && scn.events[4].value == -12.5);
    CHECK(scn.events[5].signal == OTK_SIGNAL_END);
    CHECK(scn.has_end && scn.end == 20000000000U);
}

static void expect_error(const char *text, const char *fragment) {
    char err[200] = "";
    CHECK(!otk_scenario_parse(text, &TABLE, &scn, err, sizeof(err)));
    CHECK(strstr(err, fragment) != NULL);
    if (strstr(err, fragment) == NULL) {
        fprintf(stderr, "  got: '%s', wanted '%s'\n", err, fragment);
    }
    CHECK(scn.n == 0U && !scn.has_end); /* a failed parse holds nothing */
}

static void test_rejects_bad_text(void) {
    expect_error("time_s,signal,value\n0,key_on,1\n1,kye_on,1\n", "line 3: unknown signal 'kye_on'");
    expect_error("time_s,signal,value\n2,key_on,1\n1,start,1\n", "line 3: time 1 s is earlier");
    expect_error("0,key_on,1\n", "line 1: expected the header");
    expect_error("time_s,signal,value\n0,key_on,abc\n", "line 2: bad value 'abc'");
    expect_error("time_s,signal,value\n-1,key_on,1\n", "line 2: bad time '-1'");
    expect_error("time_s,signal,value\n0,key_on\n", "line 2: expected three fields");
    expect_error("time_s,signal,value\n0,key_on,1,2\n", "line 2: expected three fields");
    expect_error("time_s,signal,value\n0,end,0\n1,key_on,1\n", "line 3: event after 'end'");
    expect_error("time_s,signal,value\n0,key_on,nan\n", "line 2: bad value 'nan'");
    expect_error("time_s,signal,value\n0,key_on,inf\n", "line 2: bad value 'inf'");
    expect_error("time_s,signal,value\n4000000.001,key_on,1\n", "line 2: bad time '4000000.001'");
    expect_error("# only a comment\n", "empty scenario");
    expect_error("", "empty scenario");
}

static void test_line_too_long(void) {
    char text[400];
    strcpy(text, "time_s,signal,value\n0,key_on,");
    const size_t used = strlen(text);
    memset(text + used, '1', 300U);
    text[used + 300U] = '\0';
    expect_error(text, "line 2: line longer than 255 characters");
}

static void test_times_round_to_the_nearest_nanosecond(void) {
    const char *text =
        "time_s,signal,value\n"
        "0.0000000004,key_on,1\n" /* 0.4 ns -> 0 */
        "0.0000000006,key_on,1\n" /* 0.6 ns -> 1 */
        "0.015,key_on,1\n"        /* 0.015 is not exact in binary: still 15 ms */
        "0.1,key_on,1\n"
        "4000000,end,0\n";        /* the largest time allowed */
    char err[128] = "";
    CHECK(otk_scenario_parse(text, &TABLE, &scn, err, sizeof(err)));
    CHECK(scn.events[0].t == 0U);
    CHECK(scn.events[1].t == 1U);
    CHECK(scn.events[2].t == 15000000U);
    CHECK(scn.events[3].t == 100000000U);
    CHECK(scn.end == UINT64_C(4000000000000000));
}

static void test_millisecond_times_stay_exact(void) {
    /* open-crank's scenarios use millisecond times; each must map to exactly
     * ms * 10^6 ns, or events would move between steps after the extraction. */
    char text[64];
    char err[128];
    for (unsigned ms = 0U; ms <= 30000U; ms++) {
        (void)snprintf(text, sizeof(text), "time_s,signal,value\n%u.%03u,key_on,1\n", ms / 1000U, ms % 1000U);
        if (!otk_scenario_parse(text, &TABLE, &scn, err, sizeof(err)) || scn.events[0].t != (OtkNs)ms * 1000000U) {
            CHECK(false);
            fprintf(stderr, "  %u ms parsed as %llu ns\n", ms, (unsigned long long)scn.events[0].t);
            return;
        }
    }
    CHECK(true);
}

static void test_event_capacity(void) {
    const size_t line = 16U;
    char *text = malloc(32U + (OTK_SCENARIO_MAX_EVENTS + 1U) * line);
    CHECK(text != NULL);
    if (text == NULL) {
        return;
    }
    size_t used = (size_t)sprintf(text, "time_s,signal,value\n");
    for (unsigned i = 0U; i < OTK_SCENARIO_MAX_EVENTS; i++) {
        used += (size_t)sprintf(text + used, "%u,key_on,1\n", i);
    }
    char err[128] = "";
    CHECK(otk_scenario_parse(text, &TABLE, &scn, err, sizeof(err)));
    CHECK(scn.n == OTK_SCENARIO_MAX_EVENTS);

    (void)sprintf(text + used, "%u,key_on,1\n", OTK_SCENARIO_MAX_EVENTS);
    expect_error(text, "more than 1024 events");
    free(text);
}

static void test_rejects_a_broken_table(void) {
    static const char *const dup[] = { "a", "a" };
    const OtkSignalTable bad = { dup, 2U };
    char err[200] = "";
    CHECK(!otk_scenario_parse("time_s,signal,value\n0,end,0\n", &bad, &scn, err, sizeof(err)));
    CHECK(strstr(err, "signal table: signal 1: duplicate name 'a'") != NULL);
}

static void test_load_errors(const char *bad_file, const char *nul_file) {
    char err[600] = "";
    CHECK(!otk_scenario_load("/nonexistent/scenario.csv", &TABLE, &scn, err, sizeof(err)));
    CHECK(strstr(err, "/nonexistent/scenario.csv: cannot open") != NULL);

    CHECK(!otk_scenario_load(bad_file, &TABLE, &scn, err, sizeof(err)));
    CHECK(strstr(err, "bad_signal.csv: line 4: unknown signal 'tagret_rpm'") != NULL);

    FILE *fp = fopen(nul_file, "wb");
    CHECK(fp != NULL);
    if (fp != NULL) {
        static const char bytes[] = "time_s,signal,value\n0,key_on,1\n\0garbage\n";
        (void)fwrite(bytes, 1U, sizeof(bytes) - 1U, fp);
        fclose(fp);
        CHECK(!otk_scenario_load(nul_file, &TABLE, &scn, err, sizeof(err)));
        CHECK(strstr(err, "not a text file") != NULL);
        remove(nul_file);
    }
}

/* Usage: test_scenario <data dir> <scratch dir> */
int main(int argc, char **argv) {
    RUN_TEST(test_parses_valid_text);
    RUN_TEST(test_rejects_bad_text);
    RUN_TEST(test_line_too_long);
    RUN_TEST(test_times_round_to_the_nearest_nanosecond);
    RUN_TEST(test_millisecond_times_stay_exact);
    RUN_TEST(test_event_capacity);
    RUN_TEST(test_rejects_a_broken_table);
    if (argc == 3) {
        char good[512];
        char bad[512];
        char nul[512];
        (void)snprintf(good, sizeof(good), "%s/valid.csv", argv[1]);
        (void)snprintf(bad, sizeof(bad), "%s/bad_signal.csv", argv[1]);
        (void)snprintf(nul, sizeof(nul), "%s/otk_nul_scenario.csv", argv[2]);

        const int before = otk_failures;
        char err[600] = "";
        CHECK(otk_scenario_load(good, &TABLE, &scn, err, sizeof(err)));
        CHECK(scn.n == 4U && scn.has_end && scn.end == 2000000000U);
        test_load_errors(bad, nul);
        printf("%s  scenario files\n", (otk_failures == before) ? "PASS" : "FAIL");
    } else {
        CHECK(false);
        fprintf(stderr, "usage: test_scenario <data dir> <scratch dir>\n");
    }
    return check_report();
}
