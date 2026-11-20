/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file driver.h
 * @brief Drivers: what decides how fast simulated time passes.
 *
 * Every driver is the same short loop around the same step code:
 *
 *     OtkDriver d;
 *     otk_driver_init(&d, OTK_DRIVER_PACED, step);
 *     while (!done) {
 *         machine_step(...);       // advances simulated time by `step`
 *         otk_driver_wait(&d);     // free-run: returns at once; paced: sleeps
 *     }
 *
 * so switching between a deterministic CI run and an interactive SIL run is
 * a command-line flag, not a second program.
 *
 * | Driver   | Pace set by                | Deterministic | Status  |
 * |----------|----------------------------|---------------|---------|
 * | free-run | the CPU, as fast as it can | bit for bit   | v0.1    |
 * | paced    | the host's wall clock      | no            | v0.1    |
 * | virtual  | an emulator, in lockstep   | bit for bit   | Track R |
 * | hard     | a bridge's oscillator      | no: measured  | v0.4    |
 */

#ifndef OPENTICK_HOST_DRIVER_H
#define OPENTICK_HOST_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include <opentick/host/pacer.h>
#include <opentick/timebase.h>

typedef enum {
    OTK_DRIVER_FREE_RUN = 0,
    OTK_DRIVER_PACED
} OtkDriverKind;

typedef struct {
    OtkDriverKind kind;
    OtkNs step;     /**< simulated time per step */
    uint64_t steps; /**< completed waits, i.e. steps */
    OtkPacer pacer; /**< used by OTK_DRIVER_PACED only */
} OtkDriver;

/** @brief Sets up a driver for steps of @p step simulated nanoseconds. */
void otk_driver_init(OtkDriver *d, OtkDriverKind kind, OtkNs step);

/** @brief Call once after every step. */
void otk_driver_wait(OtkDriver *d);

/** @brief "free-run" or "paced". */
const char *otk_driver_name(OtkDriverKind kind);

/** @brief Parses "free-run" or "paced", for command lines. */
bool otk_driver_from_name(const char *name, OtkDriverKind *kind);

#endif /* OPENTICK_HOST_DRIVER_H */
