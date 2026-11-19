/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file signals.h
 * @brief Signal tables: the names a scenario may set, registered by the
 *        application.
 *
 * open-tick does not know what a machine's signals are. The application
 * gives it a table of names; a signal's id is its index in that table, so an
 * application can keep an enum that matches its table:
 *
 *     enum { SIG_THROTTLE, SIG_LOAD, SIG_COUNT };
 *     static const char *const NAMES[SIG_COUNT] = {
 *         [SIG_THROTTLE] = "throttle_pct",
 *         [SIG_LOAD] = "load_nm",
 *     };
 *     static const OtkSignalTable TABLE = { NAMES, SIG_COUNT };
 *
 * Names are made of letters, digits, '_', '.' and '-', must be unique, and
 * must not be `end`, which open-tick reserves to mark the end of a scenario.
 * Fault-injection signals are ordinary signals; by convention their names
 * start with `fault.`.
 */

#ifndef OPENTICK_SIGNALS_H
#define OPENTICK_SIGNALS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Index of a signal in its application's table. */
typedef uint32_t OtkSignalId;

/** @brief Reserved id of the `end` event. Never passed to an application. */
#define OTK_SIGNAL_END ((OtkSignalId)UINT32_MAX)

/** @brief The reserved name of the `end` event. */
#define OTK_SIGNAL_END_NAME "end"

/** @brief An application's signals, by name. */
typedef struct {
    const char *const *names; /**< names[i] is the scenario name of signal i */
    size_t count;             /**< number of names, < OTK_SIGNAL_END */
} OtkSignalTable;

/**
 * @brief Checks that a table is usable: names present, well formed, unique,
 *        and none of them `end`.
 * @param err receives a message such as "signal 3: duplicate name 'load_nm'"
 * @return true if the table is valid
 */
bool otk_signal_table_check(const OtkSignalTable *t, char *err, size_t err_len);

/** @brief Looks a signal up by name. `end` is found as OTK_SIGNAL_END. */
bool otk_signal_find(const OtkSignalTable *t, const char *name, OtkSignalId *id);

/** @brief Name of a signal: `end` for OTK_SIGNAL_END, "?" for an unknown id. */
const char *otk_signal_name(const OtkSignalTable *t, OtkSignalId id);

#endif /* OPENTICK_SIGNALS_H */
