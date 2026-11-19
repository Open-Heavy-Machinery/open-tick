/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file pacer.h
 * @brief Wall-clock pacing (soft real time) on POSIX hosts.
 *
 * The pacer only slows a loop down to one iteration per period of wall-clock
 * time; simulated time stays with whoever owns it. Deadlines are absolute,
 * so jitter does not accumulate. A loop that falls behind is not made to
 * catch up with a burst of iterations: the late iteration is counted and the
 * schedule restarts from now.
 *
 * The pacer also measures how late it woke up, so a paced run can state its
 * timing instead of assuming it.
 *
 * Extracted from open-crank v0.1 (platforms/linux-socketcan/pacer.c); the
 * period is now in nanoseconds and lateness is measured.
 */

#ifndef OPENTICK_HOST_PACER_H
#define OPENTICK_HOST_PACER_H

#include <stdint.h>
#include <time.h>

#include <opentick/timebase.h>

typedef struct {
    struct timespec next; /**< next deadline, CLOCK_MONOTONIC */
    OtkNs period;
    uint32_t overruns;    /**< iterations that started after their deadline */
    OtkNs last_late;      /**< how late the last wake-up was */
    OtkNs max_late;       /**< latest wake-up observed */
} OtkPacer;

/** @brief Starts the schedule now. @p period must be at least 1 ns. */
void otk_pacer_init(OtkPacer *p, OtkNs period);

/** @brief Sleeps until the next deadline, or counts an overrun if it has passed. */
void otk_pacer_wait(OtkPacer *p);

#endif /* OPENTICK_HOST_PACER_H */
