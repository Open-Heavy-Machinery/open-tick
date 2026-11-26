/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_hard.c
 * @brief The hard driver's host side (opentick/host/hard.h) without a board:
 *        the forwarding-cycle estimate and the clock mapping on synthetic
 *        data, and a whole transaction against a fake bridge on a
 *        pseudo-terminal (banner, stale reply, split reply, timeout).
 */

#define _GNU_SOURCE /* openpty, ptsname */

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <pty.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "check.h"
#include <opentick/bridge.h>
#include <opentick/host/hard.h>

/* A small deterministic PRNG (xorshift32). */
static uint32_t rng = 2463534242u;
static double uniform(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return (double)rng / 4294967296.0;
}

static void test_cycle_fit(void) {
    /* Replies on a 5999.2 us cycle, +/-50 us of jitter, some cycles skipped. */
    enum { N = 2000 };
    static uint64_t t[N];
    const double p = 5999200.0, t0 = 1.0e12;
    double k = 0;
    for (int i = 0; i < N; i++) {
        k += (i % 7 == 3) ? 2 : ((i % 13 == 5) ? 3 : 1);
        t[i] = (uint64_t)(t0 + k * p + (uniform() - 0.5) * 100000.0);
    }
    double period = 0, start = 0, rms = 0;
    CHECK(otk_hard_cycle_fit(t, N, 6000000.0, &period, &start, &rms));
    CHECK_NEAR(period, p, 50.0);     /* 50 ns over a 6 ms cycle */
    CHECK(rms > 20000.0 && rms < 40000.0); /* uniform +/-50 us: rms 28.9 us */
    /* t0 is the fitted grid's origin: a reply lands on it within the jitter */
    const double phase = fmod((double)t[0] - start, period);
    CHECK(phase < 60000.0 || phase > period - 60000.0);
    CHECK(!otk_hard_cycle_fit(t, 7, 6000000.0, &period, &start, &rms)); /* too few */
}

/* A bridge clock: ticks = (host_ns + offset_ns) * (1 + drift) * 27e6 / 1e9. */
static uint64_t bridge_ticks(double host_ns, double offset_ns, double drift) {
    return (uint64_t)llround((host_ns + offset_ns) * (1.0 + drift) * 0.027);
}

static void test_sync(void) {
    OtkHardSync s;
    otk_hard_sync_init(&s, 27000000u, 100000000u); /* bins of 100 ms */
    const double off = -3.0e9, drift = 6e-6;
    const double t_base = 5.0e12;
    for (int i = 0; i < 3000; i++) {
        const double t1 = t_base + i * 6.0e6;
        /* symmetric small delays most of the time; now and then a 6 ms wait down */
        const double up = 40000.0 + uniform() * 20000.0;
        const double down = (i % 3 == 0) ? 6000000.0 : 40000.0 + uniform() * 20000.0;
        const double inside = 26000.0;
        const OtkHardSample x = {
            (uint64_t)t1, (uint64_t)(t1 + up + inside + down),
            bridge_ticks(t1 + up, off, drift), bridge_ticks(t1 + up + inside, off, drift),
        };
        otk_hard_sync_add(&s, &x);
    }
    CHECK(s.fitted && s.used >= 20u);
    /* the mapping at a time inside the window is right within its bound */
    const uint64_t t_test = (uint64_t)(t_base + 2990.0 * 6.0e6);
    const double want = (double)bridge_ticks((double)t_test, off, drift);
    const double got = otk_hard_sync_to_bridge(&s, t_test);
    const double err_ns = fabs(got - want) / 0.027;
    const double bound = otk_hard_sync_bound_ns(&s);
    CHECK(err_ns <= bound);
    CHECK(bound < 100000.0); /* slow bins are left out: the bound stays below 100 us */
    CHECK_NEAR(s.b, drift, 2e-6);
    /* host -> bridge -> host */
    const uint64_t back = otk_hard_sync_to_host(&s, got);
    CHECK(back + 2u >= t_test && back <= t_test + 2u);
}

static void test_sync_leaves_out_slow_bins(void) {
    OtkHardSync s;
    otk_hard_sync_init(&s, 27000000u, 100000000u);
    for (int i = 0; i < 400; i++) {
        const double t1 = 1.0e12 + i * 6.0e6;
        /* every other 100 ms bin holds only replies that waited a whole cycle */
        const bool slow_bin = ((uint64_t)(i * 6.0e6) / 100000000u) % 2 == 1;
        const double down = slow_bin ? 6000000.0 : 50000.0;
        const OtkHardSample x = { (uint64_t)t1, (uint64_t)(t1 + 50000.0 + 26000.0 + down),
                                  bridge_ticks(t1 + 50000.0, 0, 0), bridge_ticks(t1 + 76000.0, 0, 0) };
        otk_hard_sync_add(&s, &x);
    }
    CHECK(s.fitted && s.used >= 10u && s.used <= 20u); /* about half of the 32 bins */
    CHECK(otk_hard_sync_bound_ns(&s) < 100000.0);
}

/* ---------------------------------------------------------------- a fake bridge on a pty */

typedef struct {
    int fd;
    atomic_int mode; /* 0: banner, a stale reply, then the reply in pieces; 1: silent */
} Fake;

static void *fake_bridge(void *arg) {
    Fake *f = arg;
    OtkBridgeParser p;
    otk_bridge_parser_init(&p);
    uint8_t buf[256];
    for (;;) {
        const ssize_t n = read(f->fd, buf, sizeof(buf));
        if (n <= 0) {
            return NULL;
        }
        size_t at = 0, used = 0;
        OtkBridgeMessage m;
        while (at < (size_t)n) {
            const bool got = otk_bridge_parse(&p, &buf[at], (size_t)n - at, &used, &m);
            at += used;
            if (!got || atomic_load(&f->mode) == 1) {
                continue;
            }
            /* a banner, then an old reply with another seq, then the real one in two pieces */
            const char *banner = "Console \xA5\x01 noise\r\n";
            (void)!write(f->fd, banner, strlen(banner));
            OtkBridgeReply r;
            memset(&r, 0, sizeof(r));
            r.t2 = 1000;
            r.t3 = 1700;
            r.state = OTK_BRIDGE_CONFIGURED;
            uint8_t pay[64], fr[128];
            const size_t len = otk_bridge_encode_reply(&r, pay, sizeof(pay));
            size_t fl = otk_bridge_frame(OTK_BRIDGE_EXCHANGE | OTK_BRIDGE_REPLY, (uint16_t)(m.seq - 1u), pay, len, fr, sizeof(fr));
            (void)!write(f->fd, fr, fl);
            r.fill = 7;
            fl = otk_bridge_frame(OTK_BRIDGE_EXCHANGE | OTK_BRIDGE_REPLY, m.seq, pay, otk_bridge_encode_reply(&r, pay, sizeof(pay)), fr,
                                  sizeof(fr));
            (void)!write(f->fd, fr, 10);
            usleep(2000);
            (void)!write(f->fd, &fr[10], fl - 10);
        }
    }
}

static void test_transact(void) {
    int master = -1, slave = -1;
    char name[256];
    if (openpty(&master, &slave, name, NULL, NULL) != 0) {
        printf("skip: no pseudo-terminal\n");
        return;
    }
    struct termios t;
    tcgetattr(master, &t);
    cfmakeraw(&t);
    tcsetattr(master, TCSANOW, &t);

    OtkHardLink l;
    CHECK(otk_hard_open(&l, name, 3000000u) == 0);
    close(slave);

    Fake f;
    f.fd = master;
    atomic_init(&f.mode, 0);
    pthread_t th;
    pthread_create(&th, NULL, fake_bridge, &f);

    OtkHardReply r;
    OtkBridgeReply d;
    const uint8_t empty[5] = { 0, 0, 0, 0, 0 }; /* first_step 0, n 0 */
    for (int i = 0; i < 3; i++) {
        CHECK(otk_hard_transact(&l, OTK_BRIDGE_EXCHANGE, empty, sizeof(empty), false, 1000000000u, &r) == 0);
        CHECK(r.seq == l.seq && r.type == (OTK_BRIDGE_EXCHANGE | OTK_BRIDGE_REPLY));
        CHECK(otk_bridge_decode_reply(r.payload, r.len, &d) && d.fill == 7u); /* the real one, not the stale one */
        CHECK(r.t4 >= r.t1 + 2000000u); /* T4 is when the last piece arrived */
    }
    CHECK(l.parser.discarded > 0u); /* the banners were skipped */
    /* a retransmission keeps the seq */
    const uint16_t seq = l.seq;
    CHECK(otk_hard_transact(&l, OTK_BRIDGE_EXCHANGE, empty, sizeof(empty), true, 1000000000u, &r) == 0 && r.seq == seq);

    /* silence: a timeout, counted */
    atomic_store(&f.mode, 1);
    const uint64_t t0 = otk_hard_now_ns();
    CHECK(otk_hard_transact(&l, OTK_BRIDGE_EXCHANGE, empty, sizeof(empty), false, 50000000u, &r) == 1);
    const uint64_t took = otk_hard_now_ns() - t0;
    CHECK(took >= 50000000u && took < 500000000u);
    CHECK(l.timeouts == 1u);

    otk_hard_close(&l);     /* the fake's read on the master side then fails, and it returns */
    pthread_join(th, NULL);
    close(master);
}

static void test_open_refuses_a_bad_rate(void) {
    OtkHardLink l;
    CHECK(otk_hard_open(&l, "/dev/null", 12345u) == -1);
}

int main(void) {
    RUN_TEST(test_cycle_fit);
    RUN_TEST(test_sync);
    RUN_TEST(test_sync_leaves_out_slow_bins);
    RUN_TEST(test_transact);
    RUN_TEST(test_open_refuses_a_bad_rate);
    return check_report();
}
