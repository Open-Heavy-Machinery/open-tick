/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file oracle.h
 * @brief Acceptance oracle: grades a run from the frames a device under test
 *        sends, against criteria written as data.
 *
 * The oracle is generic. It knows nothing about any application or protocol:
 * it uses only the C standard library and open-tick's core types (time, CAN
 * frames, scenarios, signal tables). The application gives it a table of
 * observables and a function that decodes a CAN frame into updates of those
 * observables; criteria name observables and scenario events.
 *
 * Criteria file (format: docs/criteria-format.md):
 *
 *     # comment lines and blank lines are ignored
 *     id,from,until,tolerance_ms,condition,text
 *     S2,start=1+2000ms,end,0,speed_control == 1,speed control taken within 2 s of start and kept
 *
 * - `from` and `until` bound an inclusive window. Each is `begin` (t = 0),
 *   `end` (the scenario's `end` event), a time in seconds (`5.0`), or
 *   `<signal>=<value>` (the first scenario event that sets that signal to
 *   that value), optionally followed by an offset `+Nms` or `-Nms`.
 * - `condition` is `term (&& term)*`, optionally followed by
 *   `-> term (&& term)*` (implication). A term is `observable op number`,
 *   op one of `== != < <= > >=`. A term on a gap is false.
 * - The criterion fails if, at steps inside its window, the condition is
 *   false for consecutive steps that cover more than `tolerance_ms` (each
 *   step covers one step period). With 0, one false step fails.
 * - `text` is everything after the fifth comma. No quoting, ever.
 *
 * Memory is fixed: no malloc. An OtkOracle is large (tens of KiB); keep it
 * off the stack.
 */

#ifndef OPENTICK_ORACLE_H
#define OPENTICK_ORACLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <opentick/can.h>
#include <opentick/scenario.h>
#include <opentick/signals.h>
#include <opentick/timebase.h>

#define OTK_ORACLE_MAX_CRITERIA 64U    /**< most criteria in one file */
#define OTK_ORACLE_MAX_OBSERVABLES 16U /**< most observables in one table */
#define OTK_ORACLE_MAX_TERMS 8U        /**< most terms in one condition, both sides of `->` together */
#define OTK_ORACLE_MAX_ID 16U          /**< longest id, terminator included */
#define OTK_ORACLE_MAX_TEXT 256U       /**< longest text, terminator included */
#define OTK_ORACLE_MAX_LABEL 64U       /**< longest event reference kept for the report, terminator included */
#define OTK_ORACLE_MAX_LINE 512U       /**< longest line of a criteria file, newline included */

/** @brief A number, or a gap (no value). */
typedef struct {
    bool gap;
    double value;
} OtkOracleValue;

/** @brief One observable. Its default holds before the first update and once the last update is older than stale. */
typedef struct {
    const char *name;
    OtkNs stale;
    OtkOracleValue fallback; /**< the default value */
} OtkOracleObservable;

/** @brief A new value for one observable, decoded from a frame. */
typedef struct {
    size_t observable; /**< index in the table */
    OtkOracleValue value;
} OtkOracleUpdate;

/**
 * @brief Decodes one frame into zero or more updates.
 * @return number of updates written to @p out (at most @p cap)
 */
typedef size_t (*OtkOracleDecodeFn)(void *ctx, const OtkCanFrame *f, OtkOracleUpdate *out, size_t cap);

/** @brief The application's observables and its frame decoder. */
typedef struct {
    const OtkOracleObservable *observables;
    size_t count;            /**< 1..OTK_ORACLE_MAX_OBSERVABLES */
    OtkOracleDecodeFn decode;
    void *ctx;               /**< passed to decode */
} OtkOracleTable;

typedef enum {
    OTK_ORACLE_EQ = 0,
    OTK_ORACLE_NE,
    OTK_ORACLE_LT,
    OTK_ORACLE_LE,
    OTK_ORACLE_GT,
    OTK_ORACLE_GE
} OtkOracleOp;

typedef struct {
    size_t observable;
    OtkOracleOp op;
    double rhs;
} OtkOracleTerm;

/** @brief One criterion, as loaded, and its grading so far. */
typedef struct {
    char id[OTK_ORACLE_MAX_ID];
    char text[OTK_ORACLE_MAX_TEXT];
    int64_t from_ns;
    int64_t until_ns;
    int64_t tolerance_ns;
    OtkOracleTerm terms[OTK_ORACLE_MAX_TERMS];
    size_t n_terms;
    size_t n_antecedent;       /**< terms before `->`; 0 without implication */

    bool from_event;           /**< `from` is an event with a positive offset: reaction time is measured */
    int64_t event_ns;
    char event[OTK_ORACLE_MAX_LABEL];

    /* Grading. */
    bool evaluated;            /**< at least one step fell inside the window */
    int64_t last_ns;           /**< last step inside the window */
    bool failed;
    int64_t fail_ns;           /**< first step of the false run that failed */
    OtkOracleValue fail_values[OTK_ORACLE_MAX_OBSERVABLES];
    bool false_run;
    int64_t false_since_ns;
    OtkOracleValue false_values[OTK_ORACLE_MAX_OBSERVABLES];
    bool reaction_done;
    bool reaction_holding;     /**< the condition held at `from` */
    int64_t true_since_ns;     /**< start of the current uninterrupted true run since the event, -1 if none */
} OtkOracleCriterion;

typedef struct {
    const OtkOracleTable *table;
    OtkNs step;
    OtkOracleCriterion criteria[OTK_ORACLE_MAX_CRITERIA];
    size_t n;

    OtkOracleValue last[OTK_ORACLE_MAX_OBSERVABLES]; /**< last decoded value */
    bool seen[OTK_ORACLE_MAX_OBSERVABLES];
    OtkNs last_ns[OTK_ORACLE_MAX_OBSERVABLES];
    OtkOracleValue now[OTK_ORACLE_MAX_OBSERVABLES];  /**< values at the last step, defaults applied */
} OtkOracle;

/**
 * @brief Parses criteria text and resolves its event references.
 *
 * @param table     the application's observables; must outlive the oracle
 * @param scenario  the scenario the run plays
 * @param signals   the signal table the scenario was loaded with
 * @param step      time between two calls of otk_oracle_step(), at least 1 ns
 * @param err       receives a message such as "line 3: unknown observable 'rpm'" on failure
 * @return false on any format error; the oracle then holds no criteria
 */
bool otk_oracle_parse(OtkOracle *o, const char *text, const OtkOracleTable *table, const OtkScenario *scenario,
                      const OtkSignalTable *signals, OtkNs step, char *err, size_t err_len);

/** @brief Reads a criteria file and parses it as otk_oracle_parse() does. */
bool otk_oracle_load(OtkOracle *o, const char *path, const OtkOracleTable *table, const OtkScenario *scenario,
                     const OtkSignalTable *signals, OtkNs step, char *err, size_t err_len);

/** @brief Hands the oracle one frame seen on the bus at @p t. */
void otk_oracle_frame(OtkOracle *o, const OtkCanFrame *f, OtkNs t);

/** @brief Evaluates every criterion once, at step @p t, after that step's frames. */
void otk_oracle_step(OtkOracle *o, OtkNs t);

/**
 * @brief Prints one line per criterion.
 * @return true if every criterion passed
 */
bool otk_oracle_report(const OtkOracle *o, FILE *fp);

/** @brief Number of criteria loaded. */
size_t otk_oracle_count(const OtkOracle *o);

/** @brief Number of criteria that failed (a window the run never covered to its end counts as failed). */
size_t otk_oracle_failures(const OtkOracle *o);

/** @brief True if the criterion @p id exists and failed. */
bool otk_oracle_failed(const OtkOracle *o, const char *id);

#endif /* OPENTICK_ORACLE_H */
