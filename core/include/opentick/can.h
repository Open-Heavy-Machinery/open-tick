/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file can.h
 * @brief CAN and CAN FD frames, and a bounded frame queue.
 *
 * A frame carries an identifier and bytes; open-tick never interprets them.
 * What a frame means (a J1939 PGN, a CANopen object) belongs to the
 * application.
 *
 * The queue is the in-memory bridge's building block: a machine and a
 * device under test in one process exchange frames through two of them,
 * with every frame still going through the application's real encoders and
 * decoders. Only the socket is missing.
 */

#ifndef OPENTICK_CAN_H
#define OPENTICK_CAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Largest payload: 8 bytes for classic CAN, 64 for CAN FD. */
#define OTK_CAN_MAX_LEN 64U

/** @name Frame flags
 * @{ */
#define OTK_CAN_EXTENDED 0x01U /**< 29-bit identifier; otherwise 11-bit */
#define OTK_CAN_FD 0x02U       /**< CAN FD frame */
#define OTK_CAN_BRS 0x04U      /**< CAN FD bit-rate switch (only with OTK_CAN_FD) */
/** @} */

#define OTK_CAN_SFF_MASK 0x000007FFUL /**< largest 11-bit identifier */
#define OTK_CAN_EFF_MASK 0x1FFFFFFFUL /**< largest 29-bit identifier */

typedef struct {
    uint32_t id;    /**< 11- or 29-bit identifier, without flag bits */
    uint8_t flags;  /**< OTK_CAN_* */
    uint8_t len;    /**< payload length in bytes */
    uint8_t data[OTK_CAN_MAX_LEN];
} OtkCanFrame;

/**
 * @brief True if @p len is a valid payload length: 0..8 for classic CAN;
 *        0..8, 12, 16, 20, 24, 32, 48 or 64 for CAN FD.
 */
bool otk_can_len_valid(bool fd, uint8_t len);

/**
 * @brief True if a frame can exist on a bus: identifier within its format,
 *        valid length, no unknown flags, BRS only on CAN FD.
 */
bool otk_can_frame_valid(const OtkCanFrame *f);

/** @brief Most frames a queue can be configured to hold. */
#define OTK_CAN_QUEUE_MAX 64U

/**
 * @brief Bounded FIFO of frames. A frame pushed into a full queue is lost
 *        and counted, as on a real controller whose mailboxes are full.
 */
typedef struct {
    OtkCanFrame frames[OTK_CAN_QUEUE_MAX];
    size_t cap;         /**< configured capacity, 1..OTK_CAN_QUEUE_MAX */
    size_t n;           /**< frames held */
    uint32_t overflows; /**< frames lost because the queue was full */
} OtkCanQueue;

/** @brief Empties the queue and sets its capacity (clamped to 1..OTK_CAN_QUEUE_MAX). */
void otk_can_queue_init(OtkCanQueue *q, size_t cap);

/** @brief Appends a frame. Returns false, and counts an overflow, if the queue is full. */
bool otk_can_queue_push(OtkCanQueue *q, const OtkCanFrame *f);

/** @brief Number of frames held. */
size_t otk_can_queue_count(const OtkCanQueue *q);

/** @brief The @p i-th oldest frame, or NULL if there is none. */
const OtkCanFrame *otk_can_queue_at(const OtkCanQueue *q, size_t i);

/** @brief Drops every frame held. Capacity and the overflow count are kept. */
void otk_can_queue_clear(OtkCanQueue *q);

#endif /* OPENTICK_CAN_H */
