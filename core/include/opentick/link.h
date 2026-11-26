/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file link.h
 * @brief A link: one direction of a CAN path, with transport faults that a
 *        scenario can switch on and off.
 *
 * A link sits between a sender and a receiver, in one direction. Every frame
 * the sender sends at time t is pushed into it; the receiver pops the frames
 * whose delivery time has come. With no fault active a link is transparent:
 * a frame pushed at t is popped at t, in the order it was pushed.
 *
 *     sender --push(frame, t)--> [ link ] --pop(now)--> receiver
 *
 * Faults (docs/link-faults.md says what each one simulates and what not):
 *
 * - drop:        every matching frame is lost;
 * - drop_one_in: N > 0 loses the Nth, 2Nth, ... matching frame;
 * - delay:       matching frames are delivered later, by a fixed time;
 * - jitter:      matching frames are delivered later, by 0..max, drawn from
 *                the link's own seeded PRNG, so runs repeat byte for byte;
 * - corrupt:     N > 0 XORs byte N (1-based) of matching frames with 0xFF:
 *                the CRC passed, the content is wrong. Shorter frames pass;
 * - bus_off:     nothing passes in this direction while it is on: frames
 *                pushed are lost, and so are frames whose delivery time
 *                falls while it is on.
 *
 * Each fault except bus_off has its own filter (identifier, mask, standard
 * or extended); without one it matches every frame. bus_off is the whole
 * direction.
 *
 * Order: a frame is never delivered before an earlier frame with the same
 * identifier (and format) that is still in the link, as on a real bus,
 * where one sender's frames with one identifier stay in order. Frames with
 * different identifiers may overtake each other under delay and jitter.
 *
 * Memory is fixed: the delay line holds up to OTK_CAN_QUEUE_MAX frames. A
 * frame pushed into a full line is lost and counted as an overflow.
 *
 * What happened to each frame: otk_link_push_ex() and otk_link_pop_ex() say
 * why a frame was lost, whether it was corrupted and when it was pushed, for
 * a frame log (framelog.h). otk_link_push() and otk_link_pop() are the same
 * without that report.
 */

#ifndef OPENTICK_LINK_H
#define OPENTICK_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <opentick/can.h>
#include <opentick/signals.h>
#include <opentick/timebase.h>

/** @brief The faults of a link. Also the order of its scenario signals. */
typedef enum {
    OTK_LINK_DROP,
    OTK_LINK_DROP_ONE_IN,
    OTK_LINK_DELAY,
    OTK_LINK_JITTER,
    OTK_LINK_CORRUPT,
    OTK_LINK_BUS_OFF,
    OTK_LINK_FAULT_COUNT
} OtkLinkFault;

/** @brief Which frames a fault applies to. */
typedef struct {
    bool on;         /**< false: every frame matches */
    bool extended;   /**< the frame's format: 29-bit if true, 11-bit if false */
    uint32_t id;     /**< identifier to match, under @c mask */
    uint32_t mask;   /**< identifier bits that must be equal */
} OtkLinkFilter;

/** @brief The faults' current values. All zero: no fault. */
typedef struct {
    bool drop;
    uint32_t drop_one_in; /**< 0: off */
    OtkNs delay;
    OtkNs jitter;         /**< largest extra delay */
    uint8_t corrupt;      /**< byte to flip, 1-based; 0: off */
    bool bus_off;
} OtkLinkFaults;

typedef struct {
    OtkCanFrame frame;
    OtkNs due;      /**< delivery time */
    OtkNs pushed;   /**< when the sender sent it */
    bool corrupted; /**< corrupt flipped a byte */
} OtkLinkSlot;

/** @brief Why a link lost a frame. */
typedef enum {
    OTK_LINK_LOST_NONE = 0,    /**< not lost */
    OTK_LINK_LOST_DROP,        /**< drop */
    OTK_LINK_LOST_DROP_ONE_IN, /**< drop_one_in */
    OTK_LINK_LOST_BUS_OFF,     /**< bus_off, when sent or when it fell due */
    OTK_LINK_LOST_OVERFLOW     /**< the line was full */
} OtkLinkLoss;

/** @brief What a link did to one frame. */
typedef struct {
    OtkLinkLoss lost;
    bool corrupted; /**< a byte was flipped (the frame is as delivered) */
    OtkNs pushed;   /**< when the sender sent it */
    OtkNs due;      /**< when it was due; for a frame lost when sent, the send time */
} OtkLinkInfo;

typedef struct {
    OtkLinkFilter filters[OTK_LINK_FAULT_COUNT]; /**< bus_off's is not used */
    OtkLinkFaults faults;
    OtkLinkSlot line[OTK_CAN_QUEUE_MAX]; /**< in push order */
    size_t cap;                          /**< configured capacity, 1..OTK_CAN_QUEUE_MAX */
    size_t n;                            /**< frames in flight */
    uint32_t matched_one_in;             /**< matching frames since drop_one_in last changed */
    uint64_t rng;                        /**< PRNG state */
    /* counters */
    uint32_t dropped;   /**< lost to drop or drop_one_in */
    uint32_t corrupted; /**< frames corrupted */
    uint32_t bus_off_lost; /**< lost to bus_off */
    uint32_t overflows; /**< lost because the line was full */
} OtkLink;

/**
 * @brief Empties the link, clears its faults, filters and counters, sets its
 *        capacity (clamped to 1..OTK_CAN_QUEUE_MAX) and seeds its PRNG.
 */
void otk_link_init(OtkLink *link, size_t cap, uint64_t seed);

/** @brief Sets the filter of one fault; NULL matches every frame. */
void otk_link_set_filter(OtkLink *link, OtkLinkFault fault, const OtkLinkFilter *filter);

/**
 * @brief Sets the faults' values. Frames already in flight keep their
 *        delivery time. Changing drop_one_in restarts its count.
 */
void otk_link_set_faults(OtkLink *link, const OtkLinkFaults *faults);

/**
 * @brief The sender sends @p frame at @p t.
 * @return true if the frame is in flight; false if it was lost (drop,
 *         drop_one_in, bus_off or overflow, each counted).
 */
bool otk_link_push(OtkLink *link, const OtkCanFrame *frame, OtkNs t);

/**
 * @brief otk_link_push(), and what happened to the frame in @p info (may be
 *        NULL): lost and why, or in flight with its due time and whether it
 *        was corrupted.
 */
bool otk_link_push_ex(OtkLink *link, const OtkCanFrame *frame, OtkNs t, OtkLinkInfo *info);

/**
 * @brief The next frame due at or before @p now, in delivery order (ties in
 *        push order). While bus_off is on, frames that fall due are lost and
 *        nothing is returned.
 * @return true and the frame in @p out, or false if none is due
 */
bool otk_link_pop(OtkLink *link, OtkNs now, OtkCanFrame *out);

/**
 * @brief Like otk_link_pop(), but a frame that falls due while bus_off is on
 *        is returned too, with info->lost = OTK_LINK_LOST_BUS_OFF: it is
 *        counted as lost and must not be delivered. @p info may be NULL.
 * @return true and the frame in @p out (delivered or lost, see @p info), or
 *         false if none is due
 */
bool otk_link_pop_ex(OtkLink *link, OtkNs now, OtkCanFrame *out, OtkLinkInfo *info);

/** @brief Frames in flight. */
size_t otk_link_count(const OtkLink *link);

/** @brief True while bus_off is on. */
bool otk_link_bus_off(const OtkLink *link);

/* ---- Scenario signals ----------------------------------------------------- */

/** @brief Number of scenario signals of one link. */
#define OTK_LINK_SIGNAL_COUNT 6U

/**
 * @brief The six scenario signal names of the link called @p name (a string
 *        literal), in OtkLinkFault order, for an application's signal table:
 *
 *     enum { SIG_TARGET, SIG_LINK_SPEED, SIG_COUNT = SIG_LINK_SPEED + OTK_LINK_SIGNAL_COUNT };
 *     static const char *const NAMES[SIG_COUNT] = {
 *         [SIG_TARGET] = "target_rpm",
 *         [SIG_LINK_SPEED] = OTK_LINK_SIGNAL_NAMES("speed"),
 *     };
 *
 * The values in a scenario: drop and bus_off 0 or 1; drop_one_in and corrupt
 * a whole number, 0 for off; delay_ms and jitter_ms in milliseconds.
 */
#define OTK_LINK_SIGNAL_NAMES(name)                                                                                  \
    "fault.link." name ".drop", "fault.link." name ".drop_one_in", "fault.link." name ".delay_ms",                \
        "fault.link." name ".jitter_ms", "fault.link." name ".corrupt", "fault.link." name ".bus_off"

/**
 * @brief Stores a scenario value in @p faults if @p signal is one of the six
 *        signals that start at @p base. Call it from the player's apply
 *        function, then apply @p faults with otk_link_set_faults() each step.
 *        Negative or out-of-range values are clamped; whole numbers round.
 * @return true if @p signal belongs to this link
 */
bool otk_link_signal(OtkLinkFaults *faults, OtkSignalId base, OtkSignalId signal, double value);

#endif /* OPENTICK_LINK_H */
