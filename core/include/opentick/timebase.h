/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file timebase.h
 * @brief The timebase: simulated time as an unsigned 64-bit count of
 *        nanoseconds.
 *
 * Nothing in open-tick reads a clock to decide what time it is in the
 * simulation. Simulated time is a number that whoever runs the machine
 * advances; a driver only decides how fast that happens (see host/driver.h).
 *
 * Why nanoseconds: message transports would be happy with milliseconds, but
 * signal-level transports timestamp edges, and a bridge's clock ticks in tens
 * of nanoseconds (an RP2040 at 125 MHz: 8 ns; a 27 MHz FPGA clock: 37 ns).
 * Why 64 bits: 2^64 ns is about 584 years, so time never wraps in practice
 * and comparisons need no wrap-safe arithmetic.
 *
 * Applications may keep a coarser time of their own (open-crank counts
 * milliseconds); they convert at the boundary with these helpers.
 */

#ifndef OPENTICK_TIMEBASE_H
#define OPENTICK_TIMEBASE_H

#include <stdint.h>

/** @brief Simulated time, or a duration, in nanoseconds. */
typedef uint64_t OtkNs;

#define OTK_NS_PER_US UINT64_C(1000)
#define OTK_NS_PER_MS UINT64_C(1000000)
#define OTK_NS_PER_S UINT64_C(1000000000)

/** @brief Milliseconds to nanoseconds. */
static inline OtkNs otk_ns_from_ms(uint64_t ms) {
    return ms * OTK_NS_PER_MS;
}

/** @brief Microseconds to nanoseconds. */
static inline OtkNs otk_ns_from_us(uint64_t us) {
    return us * OTK_NS_PER_US;
}

/** @brief Nanoseconds to whole milliseconds, rounded down. */
static inline uint64_t otk_ns_to_ms(OtkNs t) {
    return t / OTK_NS_PER_MS;
}

/** @brief Nanoseconds to seconds, for display and traces. */
static inline double otk_ns_to_s(OtkNs t) {
    return (double)t / 1e9;
}

#endif /* OPENTICK_TIMEBASE_H */
