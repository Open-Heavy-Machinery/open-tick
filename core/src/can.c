/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file can.c
 * @brief CAN frames and the frame queue. See can.h.
 */

#include <opentick/can.h>

#include <string.h>

bool otk_can_len_valid(bool fd, uint8_t len) {
    if (len <= 8U) {
        return true;
    }
    if (!fd) {
        return false;
    }
    switch (len) {
    case 12U:
    case 16U:
    case 20U:
    case 24U:
    case 32U:
    case 48U:
    case 64U:
        return true;
    default:
        return false;
    }
}

bool otk_can_frame_valid(const OtkCanFrame *f) {
    const uint8_t known = OTK_CAN_EXTENDED | OTK_CAN_FD | OTK_CAN_BRS;
    if ((f->flags & (uint8_t)~known) != 0U) {
        return false;
    }
    const bool fd = (f->flags & OTK_CAN_FD) != 0U;
    if ((f->flags & OTK_CAN_BRS) != 0U && !fd) {
        return false;
    }
    const uint32_t max_id = ((f->flags & OTK_CAN_EXTENDED) != 0U) ? OTK_CAN_EFF_MASK : OTK_CAN_SFF_MASK;
    return f->id <= max_id && otk_can_len_valid(fd, f->len);
}

void otk_can_queue_init(OtkCanQueue *q, size_t cap) {
    memset(q, 0, sizeof(*q));
    if (cap < 1U) {
        cap = 1U;
    }
    q->cap = (cap > OTK_CAN_QUEUE_MAX) ? OTK_CAN_QUEUE_MAX : cap;
}

bool otk_can_queue_push(OtkCanQueue *q, const OtkCanFrame *f) {
    if (q->n >= q->cap) {
        q->overflows++;
        return false;
    }
    q->frames[q->n++] = *f;
    return true;
}

size_t otk_can_queue_count(const OtkCanQueue *q) {
    return q->n;
}

const OtkCanFrame *otk_can_queue_at(const OtkCanQueue *q, size_t i) {
    return (i < q->n) ? &q->frames[i] : NULL;
}

void otk_can_queue_clear(OtkCanQueue *q) {
    q->n = 0U;
}
