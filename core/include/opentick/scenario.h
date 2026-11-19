/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file scenario.h
 * @brief Scenarios: time-ordered signal changes, written as plain CSV.
 *
 *     # comment lines and blank lines are ignored
 *     time_s,signal,value
 *     0.0,key_on,1
 *     0.5,start,1
 *     3.0,start,0
 *     8.0,hoist_demand_pct,85
 *     20.0,end,0
 *
 * Signal names come from the application's table (signals.h); `end` is
 * reserved and marks the end of the scenario (its value is ignored). Times
 * are seconds, rounded to the nearest nanosecond, and must not decrease.
 * Events at the same time keep their file order. Nothing may follow `end`.
 *
 * Unknown signals, bad numbers and out-of-order times are errors, reported
 * with their line number: a typo must fail loudly, not run a different test.
 */

#ifndef OPENTICK_SCENARIO_H
#define OPENTICK_SCENARIO_H

#include <stdbool.h>
#include <stddef.h>

#include <opentick/signals.h>
#include <opentick/timebase.h>

/** @brief Most events one scenario can hold, `end` included. */
#define OTK_SCENARIO_MAX_EVENTS 1024U

/** @brief Largest time a scenario may use, in seconds (about 46 days). */
#define OTK_SCENARIO_MAX_TIME_S 4.0e6

typedef struct {
    OtkNs t;            /**< when the event applies */
    OtkSignalId signal; /**< index in the application's table, or OTK_SIGNAL_END */
    double value;
} OtkEvent;

typedef struct {
    OtkEvent events[OTK_SCENARIO_MAX_EVENTS];
    size_t n;           /**< number of events, `end` included */
    bool has_end;
    OtkNs end;          /**< time of the `end` event, if has_end */
} OtkScenario;

/**
 * @brief Parses scenario text against an application's signal table.
 *
 * The table is checked first (otk_signal_table_check), so a broken table
 * fails here rather than misreading a scenario.
 *
 * @param err receives a message such as "line 4: unknown signal 'kye_on'" on failure
 * @return true on success; on failure @p s holds no events
 */
bool otk_scenario_parse(const char *text, const OtkSignalTable *signals, OtkScenario *s, char *err, size_t err_len);

/**
 * @brief Reads and parses a scenario file (at most 256 KiB).
 *
 * Not reentrant: it reads into a static buffer.
 */
bool otk_scenario_load(const char *path, const OtkSignalTable *signals, OtkScenario *s, char *err, size_t err_len);

#endif /* OPENTICK_SCENARIO_H */
