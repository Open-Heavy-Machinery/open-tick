/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_framelog.c
 * @brief Frame log writer: the exact text, errors, a write -> read round
 *        trip, the two times through a link in memory, and determinism.
 */

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include <opentick/framelog.h>

#define MS(x) otk_ns_from_ms(x)
#define STEP (UINT64_C(10) * OTK_NS_PER_MS) /* a constant: used in static initialisers */

static OtkCanFrame frame(uint32_t id, uint8_t flags, uint8_t len, uint8_t tag) {
    OtkCanFrame f;
    memset(&f, 0, sizeof(f));
    f.id = id;
    f.flags = flags;
    f.len = len;
    for (uint8_t i = 0U; i < len; i++) {
        f.data[i] = (uint8_t)(tag + i);
    }
    return f;
}

/* The whole file as a string (caller frees). */
static char *slurp(FILE *fp) {
    fflush(fp);
    const long n = ftell(fp);
    rewind(fp);
    char *buf = calloc((size_t)n + 1U, 1U);
    if (buf != NULL && n > 0) {
        size_t got = fread(buf, 1U, (size_t)n, fp);
        buf[got] = '\0';
    }
    return buf;
}

static const OtkFrameLogConfig SIM = { STEP, OTK_BUS_CLOCK_SIM, true, 0U, "test 1" };

static OtkFrameRow row(OtkNs t, OtkNs t_bus, OtkFrameDir dir, const char *link, const OtkCanFrame *f,
                       OtkFrameFault fault) {
    OtkFrameRow r = { t, t_bus, dir, link, f, fault };
    return r;
}

static void test_exact_text(void) {
    FILE *fp = tmpfile();
    OtkFrameLog log;
    CHECK(otk_framelog_init(&log, fp, &SIM));
    const OtkCanFrame ext = frame(0x0CF00400U, OTK_CAN_EXTENDED, 8U, 0xF0U);
    const OtkCanFrame std = frame(0x123U, 0U, 2U, 0x0AU);
    const OtkCanFrame fd = frame(0x7FFU, OTK_CAN_FD | OTK_CAN_BRS, 12U, 0U);
    const OtkCanFrame empty = frame(0x1U, 0U, 0U, 0U);
    OtkFrameRow r = row(MS(10), MS(10), OTK_FRAME_TO_DUT, "speed", &ext, OTK_FRAME_OK);
    CHECK(otk_framelog_frame(&log, &r));
    r = row(UINT64_C(1234567890000000), UINT64_C(1234567883000123), OTK_FRAME_FROM_DUT, "", &std,
            OTK_FRAME_CORRUPT);
    CHECK(otk_framelog_frame(&log, &r));
    r = row(MS(20), OTK_FRAMELOG_NO_TIME, OTK_FRAME_TO_DUT, NULL, &fd, OTK_FRAME_DROP);
    CHECK(otk_framelog_frame(&log, &r));
    r = row(0U, 0U, OTK_FRAME_FROM_DUT, "x", &empty, OTK_FRAME_OVERFLOW);
    CHECK(otk_framelog_frame(&log, &r));
    CHECK(otk_framelog_ok(&log) && log.rows == 4U);
    char *text = slurp(fp);
    const char *want = "# otk-frame-log 1; step_ns=10000000; bus_clock=sim; bus_clock_err_ns=0; source=test 1\n"
                       "t_s,t_bus_s,dir,link,id,flags,len,data,fault\n"
                       "0.010000000,0.010000000,to_dut,speed,0CF00400,x,8,F0F1F2F3F4F5F6F7,\n"
                       "1234567.890000000,1234567.883000123,from_dut,,123,,2,0A0B,corrupt\n"
                       "0.020000000,,to_dut,,7FF,fb,12,000102030405060708090A0B,drop\n"
                       "0.000000000,0.000000000,from_dut,x,001,,0,,overflow\n";
    CHECK(text != NULL && strcmp(text, want) == 0);
    if (text != NULL && strcmp(text, want) != 0) {
        fprintf(stderr, "got:\n%s", text);
    }
    free(text);
    fclose(fp);

    /* err_ns not stated, no source */
    fp = tmpfile();
    const OtkFrameLogConfig host = { STEP, OTK_BUS_CLOCK_HOST, false, 0U, NULL };
    CHECK(otk_framelog_init(&log, fp, &host));
    text = slurp(fp);
    const char *want_host = "# otk-frame-log 1; step_ns=10000000; bus_clock=host; bus_clock_err_ns=\n";
    CHECK(text != NULL && strncmp(text, want_host, strlen(want_host)) == 0);
    free(text);
    fclose(fp);
}

static void test_errors(void) {
    FILE *fp = tmpfile();
    OtkFrameLog log;
    OtkFrameLogConfig bad = SIM;
    bad.step = 0U;
    CHECK(!otk_framelog_init(&log, fp, &bad) && !otk_framelog_ok(&log));
    bad = SIM;
    bad.bus_clock = (OtkBusClock)99;
    CHECK(!otk_framelog_init(&log, fp, &bad));
    bad = SIM;
    bad.source = "a;b";
    CHECK(!otk_framelog_init(&log, fp, &bad));
    fclose(fp);

    const OtkCanFrame ok = frame(0x10U, 0U, 1U, 1U);
    OtkCanFrame invalid = frame(0x800U, 0U, 1U, 1U); /* 11-bit id out of range */
    struct {
        OtkFrameRow r;
        OtkBusClock clock;
    } cases[] = {
        { { MS(15), OTK_FRAMELOG_NO_TIME, OTK_FRAME_TO_DUT, "", &ok, OTK_FRAME_OK }, OTK_BUS_CLOCK_SIM },
        { { MS(10), OTK_FRAMELOG_NO_TIME, OTK_FRAME_TO_DUT, "", &invalid, OTK_FRAME_OK }, OTK_BUS_CLOCK_SIM },
        { { MS(10), OTK_FRAMELOG_NO_TIME, OTK_FRAME_TO_DUT, "", NULL, OTK_FRAME_OK }, OTK_BUS_CLOCK_SIM },
        { { MS(10), MS(10), OTK_FRAME_TO_DUT, "", &ok, OTK_FRAME_OK }, OTK_BUS_CLOCK_NONE },
        { { MS(10), MS(10), OTK_FRAME_TO_DUT, "", &ok, OTK_FRAME_DROP }, OTK_BUS_CLOCK_SIM },
        { { MS(10), MS(10), OTK_FRAME_TO_DUT, "a,b", &ok, OTK_FRAME_OK }, OTK_BUS_CLOCK_SIM },
        { { MS(10), MS(10), OTK_FRAME_TO_DUT, "a b", &ok, OTK_FRAME_OK }, OTK_BUS_CLOCK_SIM },
        { { MS(10), MS(10), (OtkFrameDir)7, "", &ok, OTK_FRAME_OK }, OTK_BUS_CLOCK_SIM },
        { { MS(10), MS(10), OTK_FRAME_TO_DUT, "", &ok, (OtkFrameFault)42 }, OTK_BUS_CLOCK_SIM },
    };
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fp = tmpfile();
        OtkFrameLogConfig cfg = SIM;
        cfg.bus_clock = cases[i].clock;
        CHECK(otk_framelog_init(&log, fp, &cfg));
        const long header_end = ftell(fp);
        CHECK(!otk_framelog_frame(&log, &cases[i].r));
        CHECK(!otk_framelog_ok(&log) && otk_framelog_error(&log)[0] != '\0');
        CHECK(ftell(fp) == header_end); /* nothing written for a bad row */
        /* sticky: a good row after the error is refused too */
        const OtkFrameRow good = { MS(10), OTK_FRAMELOG_NO_TIME, OTK_FRAME_TO_DUT, "", &ok, OTK_FRAME_OK };
        CHECK(!otk_framelog_frame(&log, &good) && log.rows == 0U);
        fclose(fp);
    }

    /* a lost from_dut frame keeps its bus time: it was on the bus */
    fp = tmpfile();
    CHECK(otk_framelog_init(&log, fp, &SIM));
    const OtkFrameRow lost_rx = { MS(10), MS(10), OTK_FRAME_FROM_DUT, "mc", &ok, OTK_FRAME_DROP };
    CHECK(otk_framelog_frame(&log, &lost_rx));
    fclose(fp);
}

/* A minimal reader, as a program outside open-tick would write one. */
typedef struct {
    char t[32], t_bus[32], dir[16], link[32], id[16], flags[8], len[8], data[160], fault[16];
} ReadRow;

/* Copies at most cap - 1 bytes and terminates: a field longer than its buffer is cut. */
static void copy_field(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1U;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static size_t read_rows(FILE *fp, ReadRow *rows, size_t cap, char *meta, size_t meta_cap) {
    rewind(fp);
    char line[512];
    size_t n = 0U;
    int lineno = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        lineno++;
        line[strcspn(line, "\n")] = '\0';
        if (lineno == 1) {
            copy_field(meta, meta_cap, line);
            continue;
        }
        if (lineno == 2 || n >= cap) {
            continue;
        }
        char *fields[9] = { rows[n].t, rows[n].t_bus, rows[n].dir, rows[n].link, rows[n].id,
                            rows[n].flags, rows[n].len, rows[n].data, rows[n].fault };
        const size_t sizes[9] = { 32, 32, 16, 32, 16, 8, 8, 160, 16 };
        char *p = line;
        for (int k = 0; k < 9; k++) {
            char *comma = (k < 8) ? strchr(p, ',') : NULL;
            if (comma != NULL) {
                *comma = '\0';
            }
            copy_field(fields[k], sizes[k], p);
            p = (comma != NULL) ? comma + 1 : p + strlen(p);
        }
        n++;
    }
    return n;
}

/* The in-memory path of a to_dut frame through a link: push on the machine
 * side, pop on the DUT side. */
static void log_to_dut(OtkFrameLog *log, OtkLink *link, const OtkCanFrame *f, OtkNs t) {
    OtkLinkInfo info;
    if (!otk_link_push_ex(link, f, t, &info)) {
        const OtkFrameRow r = { t, OTK_FRAMELOG_NO_TIME, OTK_FRAME_TO_DUT, "speed", f,
                                otk_frame_fault_from_link(&info) };
        (void)otk_framelog_frame(log, &r);
    }
}

static void pop_to_dut(OtkFrameLog *log, OtkLink *link, OtkNs now) {
    OtkLinkInfo info;
    OtkCanFrame out;
    while (otk_link_pop_ex(link, now, &out, &info)) {
        const bool lost = info.lost != OTK_LINK_LOST_NONE;
        const OtkFrameRow r = { info.pushed, lost ? OTK_FRAMELOG_NO_TIME : now, OTK_FRAME_TO_DUT, "speed", &out,
                                otk_frame_fault_from_link(&info) };
        (void)otk_framelog_frame(log, &r);
    }
}

static OtkLink g_link;

/* 100 steps of 10 ms: one frame per step, each with its own identifier (so
 * none waits behind another); a 300 ms delay from 0.2 s to 0.4 s, a drop at
 * 0.5 s, bus_off from 0.7 s to 0.8 s, corrupt byte 3 from 0.9 s. */
static void run(FILE *fp) {
    OtkFrameLog log;
    (void)otk_framelog_init(&log, fp, &SIM);
    otk_link_init(&g_link, OTK_CAN_QUEUE_MAX, 7U);
    for (uint32_t k = 0U; k < 100U; k++) {
        const OtkNs t = STEP * k;
        OtkLinkFaults fl;
        memset(&fl, 0, sizeof(fl));
        fl.delay = (k >= 20U && k < 40U) ? MS(300) : 0U;
        fl.drop = k == 50U;
        fl.bus_off = k >= 70U && k < 80U;
        fl.corrupt = (k >= 90U) ? 3U : 0U;
        otk_link_set_faults(&g_link, &fl);
        const OtkCanFrame f = frame(0x100U + k, 0U, 8U, (uint8_t)k);
        log_to_dut(&log, &g_link, &f, t);
        pop_to_dut(&log, &g_link, t);
    }
}

static void test_link_in_memory(void) {
    static ReadRow rows[256];
    char meta[200];
    FILE *fp = tmpfile();
    run(fp);
    const size_t n = read_rows(fp, rows, 256U, meta, sizeof(meta));
    CHECK(n == 100U);
    CHECK(strstr(meta, "bus_clock=sim; bus_clock_err_ns=0") != NULL);
    size_t delayed = 0U, dropped = 0U, bus_off = 0U, corrupt = 0U;
    for (size_t i = 0U; i < n; i++) {
        const double t = atof(rows[i].t), t_bus = atof(rows[i].t_bus);
        const int k = (int)(atof(rows[i].t) * 100.0 + 0.5);
        if (k >= 20 && k < 40) {
            /* t_bus - t = the delay, exactly (the text, not a double) */
            const long long ns_t = (long long)(t * 1e9 + 0.5), ns_bus = (long long)(t_bus * 1e9 + 0.5);
            CHECK(ns_bus - ns_t == 300000000LL);
            delayed++;
        } else if (strcmp(rows[i].fault, "drop") == 0) {
            CHECK(k == 50 && rows[i].t_bus[0] == '\0');
            dropped++;
        } else if (strcmp(rows[i].fault, "bus_off") == 0) {
            CHECK(rows[i].t_bus[0] == '\0');
            bus_off++;
        } else if (strcmp(rows[i].fault, "corrupt") == 0) {
            CHECK(k >= 90 && strcmp(rows[i].t, rows[i].t_bus) == 0);
            corrupt++;
        } else {
            CHECK(strcmp(rows[i].t, rows[i].t_bus) == 0 && rows[i].fault[0] == '\0');
        }
        CHECK(strcmp(rows[i].dir, "to_dut") == 0 && strcmp(rows[i].link, "speed") == 0);
        char want_id[8];
        snprintf(want_id, sizeof(want_id), "%03X", 0x100U + (unsigned)k);
        CHECK(strcmp(rows[i].id, want_id) == 0 && strcmp(rows[i].len, "8") == 0 && strlen(rows[i].data) == 16U);
    }
    CHECK(delayed == 20U && dropped == 1U && corrupt == 10U);
    CHECK(bus_off == 10U); /* frames 70..79, lost when sent */
    /* corrupted byte 3 is flipped in the data as delivered */
    for (size_t i = 0U; i < n; i++) {
        if (strcmp(rows[i].fault, "corrupt") == 0) {
            const int k = (int)(atof(rows[i].t) * 100.0 + 0.5);
            char want[3];
            snprintf(want, sizeof(want), "%02X", (unsigned)((uint8_t)(k + 2) ^ 0xFFU));
            CHECK(strncmp(rows[i].data + 4, want, 2U) == 0);
        }
    }
    /* rows are in delivery order: frame 20, delayed to 0.5 s, comes after frame 49 */
    size_t at20 = n, at49 = n;
    for (size_t i = 0U; i < n; i++) {
        if (strcmp(rows[i].t, "0.200000000") == 0) {
            at20 = i;
        }
        if (strcmp(rows[i].t, "0.490000000") == 0) {
            at49 = i;
        }
    }
    CHECK(at20 < n && at49 < n && at20 > at49);
    fclose(fp);
}

static void test_deterministic(void) {
    FILE *a = tmpfile(), *b = tmpfile();
    run(a);
    run(b);
    char *ta = slurp(a), *tb = slurp(b);
    CHECK(ta != NULL && tb != NULL && strlen(ta) > 100U && strcmp(ta, tb) == 0);
    free(ta);
    free(tb);
    fclose(a);
    fclose(b);
}

static void test_names(void) {
    CHECK(strcmp(otk_bus_clock_name(OTK_BUS_CLOCK_BRIDGE), "bridge") == 0);
    CHECK(otk_bus_clock_name((OtkBusClock)9) == NULL);
    CHECK(strcmp(otk_frame_fault_name(OTK_FRAME_DROP_ONE_IN), "drop_one_in") == 0);
    CHECK(strcmp(otk_frame_fault_name(OTK_FRAME_OK), "") == 0);
    OtkLinkInfo info = { OTK_LINK_LOST_NONE, true, 0U, 0U };
    CHECK(otk_frame_fault_from_link(&info) == OTK_FRAME_CORRUPT);
    info.lost = OTK_LINK_LOST_BUS_OFF; /* a corrupted frame lost later counts as lost */
    CHECK(otk_frame_fault_from_link(&info) == OTK_FRAME_BUS_OFF);
    info.lost = OTK_LINK_LOST_OVERFLOW;
    CHECK(otk_frame_fault_from_link(&info) == OTK_FRAME_OVERFLOW);
}

int main(void) {
    RUN_TEST(test_exact_text);
    RUN_TEST(test_errors);
    RUN_TEST(test_link_in_memory);
    RUN_TEST(test_deterministic);
    RUN_TEST(test_names);
    return check_report();
}
