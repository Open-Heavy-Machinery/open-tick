/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_can.c
 * @brief CAN frame validity and the bounded frame queue.
 */

#include <string.h>

#include "check.h"
#include <opentick/can.h>

static OtkCanFrame frame(uint32_t id, uint8_t flags, uint8_t len) {
    OtkCanFrame f;
    memset(&f, 0, sizeof(f));
    f.id = id;
    f.flags = flags;
    f.len = len;
    return f;
}

static void test_lengths(void) {
    for (uint8_t len = 0U; len <= 8U; len++) {
        CHECK(otk_can_len_valid(false, len));
        CHECK(otk_can_len_valid(true, len));
    }
    static const uint8_t fd_only[] = { 12U, 16U, 20U, 24U, 32U, 48U, 64U };
    for (size_t i = 0; i < sizeof(fd_only); i++) {
        CHECK(!otk_can_len_valid(false, fd_only[i]));
        CHECK(otk_can_len_valid(true, fd_only[i]));
    }
    static const uint8_t never[] = { 9U, 10U, 11U, 13U, 33U, 63U, 65U, 255U };
    for (size_t i = 0; i < sizeof(never); i++) {
        CHECK(!otk_can_len_valid(true, never[i]));
    }
}

static void test_frame_validity(void) {
    OtkCanFrame f = frame(0x7FFU, 0U, 8U);
    CHECK(otk_can_frame_valid(&f));
    f = frame(0x800U, 0U, 8U);
    CHECK(!otk_can_frame_valid(&f)); /* too big for 11 bits */
    f = frame(0x800U, OTK_CAN_EXTENDED, 8U);
    CHECK(otk_can_frame_valid(&f));
    f = frame(0x1FFFFFFFU, OTK_CAN_EXTENDED, 8U);
    CHECK(otk_can_frame_valid(&f));
    f = frame(0x20000000U, OTK_CAN_EXTENDED, 8U);
    CHECK(!otk_can_frame_valid(&f)); /* too big for 29 bits */
    f = frame(0x100U, 0U, 12U);
    CHECK(!otk_can_frame_valid(&f)); /* 12 bytes is CAN FD only */
    f = frame(0x100U, OTK_CAN_FD, 12U);
    CHECK(otk_can_frame_valid(&f));
    f = frame(0x100U, OTK_CAN_FD | OTK_CAN_BRS, 64U);
    CHECK(otk_can_frame_valid(&f));
    f = frame(0x100U, OTK_CAN_BRS, 8U);
    CHECK(!otk_can_frame_valid(&f)); /* bit-rate switch without FD */
    f = frame(0x100U, 0x80U, 8U);
    CHECK(!otk_can_frame_valid(&f)); /* unknown flag */
}

static void test_queue_order_and_overflow(void) {
    OtkCanQueue q;
    otk_can_queue_init(&q, 3U);
    CHECK(q.cap == 3U && otk_can_queue_count(&q) == 0U && otk_can_queue_at(&q, 0U) == NULL);

    for (uint32_t i = 0U; i < 3U; i++) {
        const OtkCanFrame f = frame(0x100U + i, 0U, 1U);
        CHECK(otk_can_queue_push(&q, &f));
    }
    const OtkCanFrame extra = frame(0x1FFU, 0U, 1U);
    CHECK(!otk_can_queue_push(&q, &extra));
    CHECK(!otk_can_queue_push(&q, &extra));
    CHECK(q.overflows == 2U);
    CHECK(otk_can_queue_count(&q) == 3U);
    CHECK(otk_can_queue_at(&q, 0U)->id == 0x100U);
    CHECK(otk_can_queue_at(&q, 2U)->id == 0x102U);
    CHECK(otk_can_queue_at(&q, 3U) == NULL);

    otk_can_queue_clear(&q);
    CHECK(otk_can_queue_count(&q) == 0U && q.cap == 3U && q.overflows == 2U);
    CHECK(otk_can_queue_push(&q, &extra));
    CHECK(otk_can_queue_at(&q, 0U)->id == 0x1FFU);
}

static void test_queue_frames_are_copies(void) {
    OtkCanQueue q;
    otk_can_queue_init(&q, 4U);
    OtkCanFrame f = frame(0x123U, OTK_CAN_FD, 64U);
    for (uint8_t i = 0U; i < 64U; i++) {
        f.data[i] = i;
    }
    CHECK(otk_can_queue_push(&q, &f));
    f.data[63] = 0xAAU;
    CHECK(otk_can_queue_at(&q, 0U)->data[63] == 63U);
}

static void test_queue_capacity_is_clamped(void) {
    OtkCanQueue q;
    otk_can_queue_init(&q, 0U);
    CHECK(q.cap == 1U);
    otk_can_queue_init(&q, 1000U);
    CHECK(q.cap == OTK_CAN_QUEUE_MAX);
}

int main(void) {
    RUN_TEST(test_lengths);
    RUN_TEST(test_frame_validity);
    RUN_TEST(test_queue_order_and_overflow);
    RUN_TEST(test_queue_frames_are_copies);
    RUN_TEST(test_queue_capacity_is_clamped);
    return check_report();
}
