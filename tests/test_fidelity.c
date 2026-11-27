/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_fidelity.c
 * @brief The fidelity contract (fidelity.h, docs/bridge-protocol.md §7): the
 *        parser, each comparison rule at its edge, the matching of rows, and
 *        the message.
 */

#include <stdio.h>
#include <string.h>

#include <opentick/fidelity.h>

#include "check.h"

#define HDR "signal,direction,transport,max_rate_hz,resolution_ns,latency_ns,jitter_ns,logic_v\n"

/* §7's example */
static const char MACHINE[] = "# otk-fidelity 1; side=machine; source=open-crank 0.4\n" HDR
                              "crank,to_dut,rotary,2400,100,,1000,3.3\n"
                              "cam,to_dut,rotary,20,100,,1000,3.3\n"
                              "inj1,from_dut,capture,20,1000,22000000,,3.3\n"
                              "oil_pressure,to_dut,analog,1000,,2000000,,3.3\n";

static const char BRIDGE[] = "# otk-fidelity 1; side=bridge; lead_ns=16000000; dut_to_plant_delay_ns=17300000; "
                             "link=bl616-3mbaud\n" HDR
                             "rotary0,to_dut,rotary,2400,37,,1000,3.3\n"
                             "rotary1,to_dut,rotary,2400,37,,1000,3.3\n"
                             "capture0,from_dut,capture,20,37,,,3.3\n"
                             "capture1,from_dut,capture,20,37,,,3.3\n"
                             "analog0,to_dut,analog,1000,,2000000,,3.3\n";

static OtkFidelityFile g_m, g_b; /* large: off the stack */

static bool parse(const char *text, OtkFidelityFile *f, OtkFidelityError *e) {
    return otk_fidelity_parse(text, strlen(text), f, e);
}

/* The check of MACHINE against BRIDGE with one line of either replaced. */
static OtkFidelityVerdict check_with(const char *machine, const char *bridge, OtkFidelityResult *r) {
    OtkFidelityError e;
    CHECK(parse(machine, &g_m, &e));
    CHECK(parse(bridge, &g_b, &e));
    return otk_fidelity_check(&g_m, &g_b, r);
}

static void test_parse_example(void) {
    OtkFidelityError e;
    CHECK(parse(MACHINE, &g_m, &e));
    CHECK(g_m.side == OTK_FIDELITY_SIDE_MACHINE && g_m.n == 4u && !g_m.has_delay);
    const OtkFidelitySignal *inj = &g_m.sig[2];
    CHECK(strcmp(inj->name, "inj1") == 0 && inj->line == 5u);
    CHECK(inj->dir == OTK_FIDELITY_FROM_DUT && inj->transport == OTK_FIDELITY_CAPTURE);
    CHECK(inj->has[OTK_FIDELITY_LATENCY_NS] && inj->value[OTK_FIDELITY_LATENCY_NS] == 22000000u);
    CHECK(!inj->has[OTK_FIDELITY_JITTER_NS]);
    CHECK(inj->has[OTK_FIDELITY_LOGIC_MV] && inj->value[OTK_FIDELITY_LOGIC_MV] == 3300u);
    CHECK(g_m.sig[3].transport == OTK_FIDELITY_ANALOG && !g_m.sig[3].has[OTK_FIDELITY_RESOLUTION_NS]);
    CHECK(parse(BRIDGE, &g_b, &e));
    CHECK(g_b.side == OTK_FIDELITY_SIDE_BRIDGE && g_b.n == 5u);
    CHECK(g_b.has_lead && g_b.lead_ns == 16000000u && g_b.has_delay && g_b.dut_to_plant_delay_ns == 17300000u);
    CHECK(strcmp(g_b.link, "bl616-3mbaud") == 0);
    /* CRLF, and blank lines at the end */
    CHECK(parse("# otk-fidelity 1; side=machine\r\n" "signal,direction,transport,max_rate_hz,resolution_ns,"
                "latency_ns,jitter_ns,logic_v\r\nx,to_dut,level,,,,,5\r\n\r\n\n",
                &g_m, &e));
    CHECK(g_m.n == 1u && g_m.sig[0].transport == OTK_FIDELITY_LEVEL && g_m.sig[0].value[OTK_FIDELITY_LOGIC_MV] == 5000u);
    CHECK(parse("# otk-fidelity 1;side=bridge\n" HDR "a,from_dut,capture,,,,,1.25\n", &g_b, &e));
    CHECK(g_b.sig[0].value[OTK_FIDELITY_LOGIC_MV] == 1250u);
}

/* A file that must be refused, at that line and column. */
static void bad(const char *text, unsigned line, const char *column) {
    OtkFidelityError e;
    const bool ok = parse(text, &g_m, &e);
    CHECK(!ok);
    CHECK(e.line == line);
    CHECK(column == NULL ? e.column == NULL : (e.column != NULL && strcmp(e.column, column) == 0));
    CHECK(e.what != NULL);
}

static void test_parse_errors(void) {
    bad("", 1u, NULL);
    bad("# otk-fidelity 2; side=machine\n" HDR, 1u, NULL);
    bad("# otk-fidelity 1\n" HDR, 1u, "side");
    bad("# otk-fidelity 1; side=dut\n" HDR, 1u, "side");
    bad("# otk-fidelity 1; side=bridge; lead_ns=16ms\n" HDR, 1u, "lead_ns");
    bad("# otk-fidelity 1; side=bridge; dut_to_plant_delay_ns=-1\n" HDR, 1u, "dut_to_plant_delay_ns");
    bad("# otk-fidelity 1; side=machine; oops\n" HDR, 1u, NULL);
    bad("# otk-fidelity 1; side=machine\n", 2u, NULL);
    bad("# otk-fidelity 1; side=machine\nsignal,direction\n", 2u, NULL);
    bad("# otk-fidelity 1; side=machine\n" HDR "a,to_dut,rotary,1,2,3,4\n", 3u, NULL);
    bad("# otk-fidelity 1; side=machine\n" HDR "a,to_dut,rotary,1,2,3,4,3.3,9\n", 3u, NULL);
    bad("# otk-fidelity 1; side=machine\n" HDR ",to_dut,rotary,,,,,\n", 3u, "signal");
    bad("# otk-fidelity 1; side=machine\n" HDR "a,to_dut,rotary,,,,,\na,to_dut,rotary,,,,,\n", 4u, "signal");
    bad("# otk-fidelity 1; side=machine\n" HDR "a,sideways,rotary,,,,,\n", 3u, "direction");
    bad("# otk-fidelity 1; side=machine\n" HDR "a,to_dut,pwm,,,,,\n", 3u, "transport");
    bad("# otk-fidelity 1; side=machine\n" HDR "a,to_dut,rotary,2.5,,,,\n", 3u, "max_rate_hz");
    bad("# otk-fidelity 1; side=machine\n" HDR "a,to_dut,rotary,,-1,,,\n", 3u, "resolution_ns");
    bad("# otk-fidelity 1; side=machine\n" HDR "a,to_dut,rotary,,,1e6,,\n", 3u, "latency_ns");
    bad("# otk-fidelity 1; side=machine\n" HDR "a,to_dut,rotary,,,,99999999999999999999,\n", 3u, "jitter_ns");
    bad("# otk-fidelity 1; side=machine\n" HDR "a,to_dut,rotary,,,,,3.3.3\n", 3u, "logic_v");
    bad("# otk-fidelity 1; side=machine\n" HDR "a,to_dut,rotary,,,,,3.3333\n", 3u, "logic_v");
    bad("# otk-fidelity 1; side=machine\n" HDR "a,to_dut,rotary,,,,,\n\nb,to_dut,rotary,,,,,\n", 5u, NULL);
}

static void test_check_example(void) {
    OtkFidelityResult r;
    CHECK(check_with(MACHINE, BRIDGE, &r) == OTK_FIDELITY_OK);
    char msg[200];
    otk_fidelity_describe(&r, &g_m, &g_b, msg, sizeof(msg));
    CHECK(strstr(msg, "every signal") != NULL);
}

/* One machine row against one bridge row: the verdict and the column. */
static OtkFidelityVerdict one(const char *mrow, const char *brow, const char *bmeta, OtkFidelityResult *r) {
    static char m[512], b[512];
    snprintf(m, sizeof(m), "# otk-fidelity 1; side=machine\n" HDR "%s\n", mrow);
    snprintf(b, sizeof(b), "# otk-fidelity 1; side=bridge%s\n" HDR "%s\n", bmeta, brow);
    return check_with(m, b, r);
}

static void test_max_rate(void) {
    OtkFidelityResult r;
    CHECK(one("s,to_dut,rotary,2400,,,,", "b,to_dut,rotary,2400,,,,", "", &r) == OTK_FIDELITY_OK);
    CHECK(one("s,to_dut,rotary,2401,,,,", "b,to_dut,rotary,2400,,,,", "", &r) == OTK_FIDELITY_NOT_MET);
    CHECK(r.column == OTK_FIDELITY_MAX_RATE_HZ && r.required == 2401u && r.offered == 2400u);
}

static void test_resolution(void) {
    OtkFidelityResult r;
    CHECK(one("s,to_dut,rotary,,100,,,", "b,to_dut,rotary,,100,,,", "", &r) == OTK_FIDELITY_OK);
    CHECK(one("s,to_dut,rotary,,100,,,", "b,to_dut,rotary,,101,,,", "", &r) == OTK_FIDELITY_NOT_MET);
    CHECK(r.column == OTK_FIDELITY_RESOLUTION_NS && r.required == 100u && r.offered == 101u);
}

static void test_jitter(void) {
    OtkFidelityResult r;
    CHECK(one("s,to_dut,rotary,,,,1000,", "b,to_dut,rotary,,,,1000,", "", &r) == OTK_FIDELITY_OK);
    CHECK(one("s,to_dut,rotary,,,,1000,", "b,to_dut,rotary,,,,1001,", "", &r) == OTK_FIDELITY_NOT_MET);
    CHECK(r.column == OTK_FIDELITY_JITTER_NS);
}

static void test_latency(void) {
    OtkFidelityResult r;
    /* to_dut: the row's own, the metadata delay plays no part */
    CHECK(one("s,to_dut,analog,,,2000000,,", "b,to_dut,analog,,,2000000,,", "; dut_to_plant_delay_ns=99999999", &r) ==
          OTK_FIDELITY_OK);
    CHECK(one("s,to_dut,analog,,,2000000,,", "b,to_dut,analog,,,2000001,,", "", &r) == OTK_FIDELITY_NOT_MET);
    CHECK(r.column == OTK_FIDELITY_LATENCY_NS && r.offered == 2000001u);
    CHECK(one("s,to_dut,analog,,,2000000,,", "b,to_dut,analog,,,,,", "; dut_to_plant_delay_ns=1", &r) ==
          OTK_FIDELITY_NOT_GUARANTEED);
    /* from_dut: the delay plus the row's own (empty counts 0) */
    CHECK(one("s,from_dut,capture,,,17300000,,", "b,from_dut,capture,,,,,", "; dut_to_plant_delay_ns=17300000", &r) ==
          OTK_FIDELITY_OK);
    CHECK(one("s,from_dut,capture,,,17300000,,", "b,from_dut,capture,,,1,,", "; dut_to_plant_delay_ns=17300000", &r) ==
          OTK_FIDELITY_NOT_MET);
    CHECK(r.column == OTK_FIDELITY_LATENCY_NS && r.required == 17300000u && r.offered == 17300001u);
    CHECK(one("s,from_dut,capture,,,17299999,,", "b,from_dut,capture,,,,,", "; dut_to_plant_delay_ns=17300000", &r) ==
          OTK_FIDELITY_NOT_MET);
    /* no delay declared: a from_dut latency is not guaranteed, whatever the row says */
    CHECK(one("s,from_dut,capture,,,22000000,,", "b,from_dut,capture,,,1,,", "", &r) == OTK_FIDELITY_NOT_GUARANTEED);
    CHECK(r.column == OTK_FIDELITY_LATENCY_NS);
}

static void test_logic_v(void) {
    OtkFidelityResult r;
    CHECK(one("s,to_dut,level,,,,,3.3", "b,to_dut,level,,,,,3.30", "", &r) == OTK_FIDELITY_OK);
    CHECK(one("s,to_dut,level,,,,,5", "b,to_dut,level,,,,,3.3", "", &r) == OTK_FIDELITY_NOT_MET);
    CHECK(r.column == OTK_FIDELITY_LOGIC_MV && r.required == 5000u && r.offered == 3300u);
    CHECK(one("s,to_dut,level,,,,,3.3", "b,to_dut,level,,,,,5", "", &r) == OTK_FIDELITY_NOT_MET);
}

static void test_not_guaranteed(void) {
    OtkFidelityResult r;
    CHECK(one("s,to_dut,rotary,,,,1000,", "b,to_dut,rotary,2400,37,,,3.3", "", &r) == OTK_FIDELITY_NOT_GUARANTEED);
    CHECK(r.column == OTK_FIDELITY_JITTER_NS && r.required == 1000u);
    /* an empty machine cell asks for nothing, even against an empty bridge cell */
    CHECK(one("s,to_dut,rotary,,,,,", "b,to_dut,rotary,,,,,", "", &r) == OTK_FIDELITY_OK);
}

static void test_matching(void) {
    OtkFidelityResult r;
    /* same transport, other direction: not a channel for it */
    CHECK(one("s,from_dut,rotary,,,,,", "b,to_dut,rotary,,,,,", "", &r) == OTK_FIDELITY_NO_CHANNEL);
    CHECK(one("s,to_dut,capture,,,,,", "b,to_dut,rotary,,,,,", "", &r) == OTK_FIDELITY_NO_CHANNEL);
    /* each bridge row is taken once, in file order */
    static const char M2[] = "# otk-fidelity 1; side=machine\n" HDR "a,from_dut,capture,,,,,\nb,from_dut,capture,,,,,\n"
                             "c,from_dut,capture,,,,,\n";
    static const char B2[] = "# otk-fidelity 1; side=bridge\n" HDR "capture0,from_dut,capture,,,,,\n"
                             "rotary0,to_dut,rotary,,,,,\ncapture1,from_dut,capture,,,,,\n";
    CHECK(check_with(M2, B2, &r) == OTK_FIDELITY_NO_CHANNEL && r.machine_row == 2u);
    /* the first free row is the one compared: here capture0, too slow, though capture1 would do */
    static const char M3[] = "# otk-fidelity 1; side=machine\n" HDR "a,from_dut,capture,100,,,,\n";
    static const char B3[] = "# otk-fidelity 1; side=bridge\n" HDR "capture0,from_dut,capture,10,,,,\n"
                             "capture1,from_dut,capture,1000,,,,\n";
    CHECK(check_with(M3, B3, &r) == OTK_FIDELITY_NOT_MET && r.bridge_row == 0u);
    /* and the second machine row gets the second bridge row */
    static const char M4[] = "# otk-fidelity 1; side=machine\n" HDR "a,from_dut,capture,10,,,,\nb,from_dut,capture,100,,,,\n";
    CHECK(check_with(M4, B3, &r) == OTK_FIDELITY_OK);
}

static void test_wrong_side(void) {
    OtkFidelityResult r;
    CHECK(check_with(BRIDGE, BRIDGE, &r) == OTK_FIDELITY_WRONG_SIDE);
    CHECK(check_with(MACHINE, MACHINE, &r) == OTK_FIDELITY_WRONG_SIDE);
}

static void test_message(void) {
    OtkFidelityResult r;
    char msg[256];
    static const char M[] = "# otk-fidelity 1; side=machine\n" HDR "crank,to_dut,rotary,2400,,,,\n"
                            "inj1,from_dut,capture,,,10000000,,3.3\n";
    CHECK(check_with(M, BRIDGE, &r) == OTK_FIDELITY_NOT_MET);
    CHECK(r.machine_row == 1u);
    otk_fidelity_describe(&r, &g_m, &g_b, msg, sizeof(msg));
    CHECK(strstr(msg, "inj1") != NULL && strstr(msg, "line 4") != NULL && strstr(msg, "latency_ns") != NULL);
    CHECK(strstr(msg, "10000000 ns") != NULL && strstr(msg, "17300000 ns") != NULL && strstr(msg, "capture0") != NULL);
    CHECK(one("s,to_dut,level,,,,,5", "b,to_dut,level,,,,,3.3", "", &r) == OTK_FIDELITY_NOT_MET);
    otk_fidelity_describe(&r, &g_m, &g_b, msg, sizeof(msg));
    CHECK(strstr(msg, "exactly 5.000 V") != NULL && strstr(msg, "3.300 V") != NULL);
    CHECK(one("s,to_dut,capture,,,,,", "b,to_dut,rotary,,,,,", "", &r) == OTK_FIDELITY_NO_CHANNEL);
    otk_fidelity_describe(&r, &g_m, &g_b, msg, sizeof(msg));
    CHECK(strstr(msg, "signal s (line 3)") != NULL && strstr(msg, "to_dut capture") != NULL);
}

int main(void) {
    RUN_TEST(test_parse_example);
    RUN_TEST(test_parse_errors);
    RUN_TEST(test_check_example);
    RUN_TEST(test_max_rate);
    RUN_TEST(test_resolution);
    RUN_TEST(test_jitter);
    RUN_TEST(test_latency);
    RUN_TEST(test_logic_v);
    RUN_TEST(test_not_guaranteed);
    RUN_TEST(test_matching);
    RUN_TEST(test_wrong_side);
    RUN_TEST(test_message);
    return check_report();
}
