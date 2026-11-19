/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_signals.c
 * @brief Signal tables registered by an application.
 */

#include <string.h>

#include "check.h"
#include <opentick/signals.h>

static const char *const GOOD[] = { "key_on", "target_rpm", "fault.mc_tx_lost", "load-nm" };
static const OtkSignalTable GOOD_TABLE = { GOOD, 4U };

static void expect_bad(const char *const *names, size_t n, const char *fragment) {
    const OtkSignalTable t = { names, n };
    char err[160] = "";
    CHECK(!otk_signal_table_check(&t, err, sizeof(err)));
    CHECK(strstr(err, fragment) != NULL);
    if (strstr(err, fragment) == NULL) {
        fprintf(stderr, "  got: '%s', wanted '%s'\n", err, fragment);
    }
}

static void test_valid_table(void) {
    char err[160] = "";
    CHECK(otk_signal_table_check(&GOOD_TABLE, err, sizeof(err)));
    CHECK(err[0] == '\0');

    const OtkSignalTable empty = { NULL, 0U };
    CHECK(otk_signal_table_check(&empty, err, sizeof(err))); /* a scenario of `end` alone */
}

static void test_find_and_name(void) {
    OtkSignalId id = 99U;
    CHECK(otk_signal_find(&GOOD_TABLE, "fault.mc_tx_lost", &id) && id == 2U);
    CHECK(otk_signal_find(&GOOD_TABLE, "key_on", &id) && id == 0U);
    CHECK(otk_signal_find(&GOOD_TABLE, "end", &id) && id == OTK_SIGNAL_END);
    CHECK(!otk_signal_find(&GOOD_TABLE, "kye_on", &id));
    CHECK(!otk_signal_find(&GOOD_TABLE, "KEY_ON", &id)); /* names are case-sensitive */

    CHECK(strcmp(otk_signal_name(&GOOD_TABLE, 1U), "target_rpm") == 0);
    CHECK(strcmp(otk_signal_name(&GOOD_TABLE, OTK_SIGNAL_END), "end") == 0);
    CHECK(strcmp(otk_signal_name(&GOOD_TABLE, 4U), "?") == 0);
}

static void test_rejects_bad_tables(void) {
    static const char *const dup[] = { "a", "b", "a" };
    expect_bad(dup, 3U, "signal 2: duplicate name 'a' (also signal 0)");

    static const char *const end[] = { "a", "end" };
    expect_bad(end, 2U, "signal 1: 'end' is reserved");

    static const char *const comma[] = { "a,b" };
    expect_bad(comma, 1U, "signal 0: 'a,b' has a character");

    static const char *const space[] = { "hoist demand" };
    expect_bad(space, 1U, "signal 0: 'hoist demand' has a character");

    static const char *const empty[] = { "a", "" };
    expect_bad(empty, 2U, "signal 1: no name");

    static const char *const null[] = { NULL };
    expect_bad(null, 1U, "signal 0: no name");

    expect_bad(NULL, 3U, "no signal table");
}

int main(void) {
    RUN_TEST(test_valid_table);
    RUN_TEST(test_find_and_name);
    RUN_TEST(test_rejects_bad_tables);
    return check_report();
}
