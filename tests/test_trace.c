/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_trace.c
 * @brief CSV trace writer: exact bytes, and every way a row can go wrong.
 */

#include <string.h>

#include "check.h"
#include <opentick/trace.h>

/* Reads back everything written to @p fp. */
static const char *contents(FILE *fp) {
    static char buf[4096];
    fflush(fp);
    rewind(fp);
    const size_t n = fread(buf, 1U, sizeof(buf) - 1U, fp);
    buf[n] = '\0';
    return buf;
}

static void expect_text(FILE *fp, const char *expected) {
    const char *got = contents(fp);
    CHECK(strcmp(got, expected) == 0);
    if (strcmp(got, expected) != 0) {
        fprintf(stderr, "  got:\n%s  wanted:\n%s", got, expected);
    }
}

/* The formats open-crank v0.1 writes: %.3f, %d, %.1f, %.4f, %s, %u, and empty
 * controller columns when the controller runs elsewhere. */
static const OtkTraceColumn COLS[] = {
    { "t_s", OTK_COL_REAL, 3 },
    { "key_on", OTK_COL_INT, 0 },
    { "rpm", OTK_COL_REAL, 2 },
    { "boost_frac", OTK_COL_REAL, 4 },
    { "ecu_state", OTK_COL_TEXT, 0 },
    { "controlling_sa", OTK_COL_UINT, 0 },
    { "mc_state", OTK_COL_TEXT, 0 },
    { "mc_target_rpm", OTK_COL_REAL, 1 },
};
#define N_COLS (sizeof(COLS) / sizeof(COLS[0]))

static void test_header_and_rows(void) {
    FILE *fp = tmpfile();
    CHECK(fp != NULL);
    if (fp == NULL) {
        return;
    }
    OtkTrace t;
    CHECK(otk_trace_init(&t, fp, COLS, N_COLS));

    otk_trace_real(&t, 8.47);
    otk_trace_int(&t, 1);
    otk_trace_real(&t, 1328.7049);
    otk_trace_real(&t, 0.123456);
    otk_trace_text(&t, "RUNNING");
    otk_trace_uint(&t, 0x27U);
    otk_trace_text(&t, "RUN");
    otk_trace_real(&t, 1500.0);
    CHECK(otk_trace_end_row(&t));

    otk_trace_real(&t, 8.48);
    otk_trace_int(&t, -3);
    otk_trace_real(&t, -0.004);
    otk_trace_real(&t, 1.0);
    otk_trace_text(&t, "");
    otk_trace_uint(&t, 255U);
    otk_trace_empty(&t);
    otk_trace_empty(&t);
    CHECK(otk_trace_end_row(&t));

    CHECK(otk_trace_ok(&t) && strcmp(otk_trace_error(&t), "") == 0);
    CHECK(t.rows == 2U);

    char line2[200];
    (void)snprintf(line2, sizeof(line2), "%.3f,%d,%.2f,%.4f,%s,%u,%s,%.1f\n", 8.47, 1, 1328.7049, 0.123456, "RUNNING",
                   0x27U, "RUN", 1500.0);
    char expected[400];
    (void)snprintf(expected, sizeof(expected),
                   "t_s,key_on,rpm,boost_frac,ecu_state,controlling_sa,mc_state,mc_target_rpm\n%s%s", line2,
                   "8.480,-3,-0.00,1.0000,,255,,\n");
    expect_text(fp, expected);
    fclose(fp);
}

static void expect_row_error(void (*fill)(OtkTrace *), const char *fragment, const char *expected_row) {
    FILE *fp = tmpfile();
    CHECK(fp != NULL);
    if (fp == NULL) {
        return;
    }
    static const OtkTraceColumn cols[] = {
        { "a", OTK_COL_REAL, 1 },
        { "b", OTK_COL_TEXT, 0 },
    };
    OtkTrace t;
    CHECK(otk_trace_init(&t, fp, cols, 2U));
    fill(&t);
    CHECK(!otk_trace_end_row(&t));
    CHECK(!otk_trace_ok(&t));
    CHECK(strstr(otk_trace_error(&t), fragment) != NULL);
    if (strstr(otk_trace_error(&t), fragment) == NULL) {
        fprintf(stderr, "  got: '%s', wanted '%s'\n", otk_trace_error(&t), fragment);
    }

    /* The row is still one line with the right number of fields. */
    char expected[200];
    (void)snprintf(expected, sizeof(expected), "a,b\n%s", expected_row);
    expect_text(fp, expected);
    fclose(fp);
}

static void fill_wrong_kind(OtkTrace *t) {
    otk_trace_int(t, 7);
    otk_trace_text(t, "x");
}
static void fill_missing(OtkTrace *t) {
    otk_trace_real(t, 1.0);
}
static void fill_extra(OtkTrace *t) {
    otk_trace_real(t, 1.0);
    otk_trace_text(t, "x");
    otk_trace_text(t, "y");
}
static void fill_comma(OtkTrace *t) {
    otk_trace_real(t, 1.0);
    otk_trace_text(t, "a,b");
}
static void fill_null_text(OtkTrace *t) {
    otk_trace_real(t, 1.0);
    otk_trace_text(t, NULL);
}
static void fill_two_errors(OtkTrace *t) {
    otk_trace_text(t, "x"); /* first error: wrong kind in 'a' */
    otk_trace_text(t, "y,z");
}

static void test_row_errors(void) {
    expect_row_error(fill_wrong_kind, "row 1: column 'a' given a value of the wrong kind", "7,x\n");
    expect_row_error(fill_missing, "row 1: column 'b' and later missing", "1.0,\n");
    expect_row_error(fill_extra, "row 1: more than 2 values", "1.0,x\n");
    expect_row_error(fill_comma, "row 1: column 'b': missing text, or a comma", "1.0,\n");
    expect_row_error(fill_null_text, "row 1: column 'b': missing text", "1.0,\n");
    expect_row_error(fill_two_errors, "column 'a' given a value of the wrong kind", "x,\n"); /* first one kept */
}

static void expect_bad_columns(const OtkTraceColumn *cols, size_t n, const char *fragment) {
    FILE *fp = tmpfile();
    CHECK(fp != NULL);
    if (fp == NULL) {
        return;
    }
    OtkTrace t;
    CHECK(!otk_trace_init(&t, fp, cols, n));
    CHECK(strstr(otk_trace_error(&t), fragment) != NULL);
    if (strstr(otk_trace_error(&t), fragment) == NULL) {
        fprintf(stderr, "  got: '%s', wanted '%s'\n", otk_trace_error(&t), fragment);
    }
    CHECK(strcmp(contents(fp), "") == 0); /* no header for a bad definition */
    fclose(fp);
}

static void test_bad_columns(void) {
    static const OtkTraceColumn unnamed[] = { { "", OTK_COL_REAL, 1 } };
    expect_bad_columns(unnamed, 1U, "column 0: missing name");
    static const OtkTraceColumn comma[] = { { "a", OTK_COL_REAL, 1 }, { "b,c", OTK_COL_REAL, 1 } };
    expect_bad_columns(comma, 2U, "column 1: missing name or a comma");
    static const OtkTraceColumn decimals[] = { { "a", OTK_COL_REAL, 18 } };
    expect_bad_columns(decimals, 1U, "column 'a': decimals must be 0..17");
    static const OtkTraceColumn kind[] = { { "a", (OtkColumnKind)42, 0 } };
    expect_bad_columns(kind, 1U, "column 'a': unknown kind");
    expect_bad_columns(NULL, 0U, "no file or no columns");
}

int main(void) {
    RUN_TEST(test_header_and_rows);
    RUN_TEST(test_row_errors);
    RUN_TEST(test_bad_columns);
    return check_report();
}
