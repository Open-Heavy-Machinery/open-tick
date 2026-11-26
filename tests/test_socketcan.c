/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_socketcan.c
 * @brief SocketCAN bridge over a real (virtual) bus.
 *
 * Needs a CAN interface, `vcan0` unless OTK_TEST_CAN_IF names another. With
 * none available the test is skipped (exit code 77), not passed:
 *
 *     sudo modprobe vcan
 *     sudo ip link add dev vcan0 type vcan
 *     sudo ip link set vcan0 mtu 72      # optional: also test CAN FD
 *     sudo ip link set up vcan0
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "check.h"
#include <opentick/host/socketcan.h>

#define SKIP 77

static OtkSocketCan tx_sock;
static OtkSocketCan rx_sock;

/* Waits up to 200 ms for one frame. */
static bool recv_wait(OtkSocketCan *s, OtkCanFrame *f) {
    for (int i = 0; i < 200; i++) {
        const int r = otk_socketcan_recv(s, f);
        if (r != 0) {
            return r == 1;
        }
        const struct timespec ms = { 0, 1000000L };
        nanosleep(&ms, NULL);
    }
    return false;
}

static void roundtrip(uint32_t id, uint8_t flags, uint8_t len) {
    OtkCanFrame out;
    memset(&out, 0, sizeof(out));
    out.id = id;
    out.flags = flags;
    out.len = len;
    for (uint8_t i = 0U; i < len; i++) {
        out.data[i] = (uint8_t)(0xA0U + i);
    }
    CHECK(otk_socketcan_send(&tx_sock, &out));

    OtkCanFrame in;
    memset(&in, 0x55, sizeof(in));
    CHECK(recv_wait(&rx_sock, &in));
    CHECK(in.id == id && in.flags == flags && in.len == len);
    CHECK(memcmp(in.data, out.data, len) == 0);
    for (size_t i = len; i < OTK_CAN_MAX_LEN; i++) {
        if (in.data[i] != 0U) {
            CHECK(false); /* bytes past the length are zero */
            break;
        }
    }
}

static void test_classic_frames(void) {
    roundtrip(0x0CF00400U, OTK_CAN_EXTENDED, 8U); /* EEC1 from address 0x00 */
    roundtrip(0x123U, 0U, 3U);                    /* an 11-bit frame is delivered too */
    roundtrip(0x7FFU, 0U, 0U);
}

static void test_invalid_frames_are_refused(void) {
    OtkCanFrame f;
    memset(&f, 0, sizeof(f));
    f.id = 0x800U; /* too big for 11 bits */
    f.len = 1U;
    errno = 0;
    CHECK(!otk_socketcan_send(&tx_sock, &f));
    CHECK(errno == EINVAL);
}

static void test_fd_frames(void) {
    OtkCanFrame f;
    memset(&f, 0, sizeof(f));
    f.id = 0x18FF2127U;
    f.flags = OTK_CAN_EXTENDED | OTK_CAN_FD;
    f.len = 64U;
    if (!tx_sock.fd_capable) {
        errno = 0;
        CHECK(!otk_socketcan_send(&tx_sock, &f));
        CHECK(errno == EMSGSIZE);
        printf("      (interface MTU is classic: CAN FD sending refused as expected, FD round trip not tested)\n");
        return;
    }
    roundtrip(0x18FF2127U, OTK_CAN_EXTENDED | OTK_CAN_FD, 64U);
    roundtrip(0x321U, OTK_CAN_FD | OTK_CAN_BRS, 12U);
}

/* Waits up to 200 ms for one frame, with what the kernel says about it. */
static bool recv_ex_wait(OtkSocketCan *s, OtkCanFrame *f, OtkSocketCanRxInfo *info) {
    for (int i = 0; i < 200; i++) {
        const int r = otk_socketcan_recv_ex(s, f, info);
        if (r != 0) {
            return r == 1;
        }
        const struct timespec ms = { 0, 1000000L };
        nanosleep(&ms, NULL);
    }
    return false;
}

/* Kernel timestamps on both sockets; the sender gets its own frame back. */
static void test_timestamps_and_own_frames(void) {
    CHECK(otk_socketcan_bus_clock(&rx_sock, false) == OTK_BUS_CLOCK_NONE);
    CHECK(otk_socketcan_bus_clock(&rx_sock, true) == OTK_BUS_CLOCK_HOST);
    CHECK(otk_socketcan_enable_timestamps(&rx_sock, false));
    CHECK(otk_socketcan_enable_timestamps(&tx_sock, true));
    CHECK(otk_socketcan_bus_clock(&rx_sock, true) == OTK_BUS_CLOCK_KERNEL);
    CHECK(otk_socketcan_bus_clock(&rx_sock, false) == OTK_BUS_CLOCK_NONE);

    const uint64_t before = otk_socketcan_clock_ns();
    OtkCanFrame out;
    memset(&out, 0, sizeof(out));
    out.id = 0x0C000027U;
    out.flags = OTK_CAN_EXTENDED;
    out.len = 8U;
    out.data[0] = 0xF1U;
    CHECK(otk_socketcan_send(&tx_sock, &out));

    OtkCanFrame in;
    OtkSocketCanRxInfo rx, own;
    CHECK(recv_ex_wait(&rx_sock, &in, &rx));
    CHECK(in.id == out.id && !rx.own && rx.has_time);
    CHECK(rx.time_ns >= before && rx.time_ns - before < UINT64_C(1000000000));

    CHECK(recv_ex_wait(&tx_sock, &in, &own));
    CHECK(in.id == out.id && own.own && own.has_time);
    CHECK(own.time_ns >= before && own.time_ns - before < UINT64_C(1000000000));
    CHECK(otk_socketcan_run_time(own.time_ns, before) == own.time_ns - before);
    CHECK(otk_socketcan_run_time(UINT64_C(5), UINT64_C(10)) == OTK_FRAMELOG_NO_TIME); /* before the anchor */

    /* the plain receive skips the socket's own frames: nothing else is waiting */
    CHECK(otk_socketcan_send(&tx_sock, &out));
    const struct timespec ms20 = { 0, 20000000L };
    nanosleep(&ms20, NULL);
    CHECK(otk_socketcan_recv(&tx_sock, &in) == 0);
    CHECK(recv_ex_wait(&rx_sock, &in, &rx) && !rx.own);
}

int main(void) {
    const char *ifname = getenv("OTK_TEST_CAN_IF");
    if (ifname == NULL || ifname[0] == '\0') {
        ifname = "vcan0";
    }
    if (!otk_socketcan_open(&tx_sock, ifname)) {
        printf("SKIP  no CAN interface '%s' (%s)\n", ifname, strerror(errno));
        return SKIP;
    }
    if (!otk_socketcan_open(&rx_sock, ifname)) {
        printf("SKIP  cannot open a second socket on '%s' (%s)\n", ifname, strerror(errno));
        otk_socketcan_close(&tx_sock);
        return SKIP;
    }
    RUN_TEST(test_classic_frames);
    RUN_TEST(test_invalid_frames_are_refused);
    RUN_TEST(test_fd_frames);
    RUN_TEST(test_timestamps_and_own_frames);
    otk_socketcan_close(&tx_sock);
    otk_socketcan_close(&rx_sock);
    otk_socketcan_close(&rx_sock); /* closing twice is safe */
    return check_report();
}
