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
    otk_socketcan_close(&tx_sock);
    otk_socketcan_close(&rx_sock);
    otk_socketcan_close(&rx_sock); /* closing twice is safe */
    return check_report();
}
