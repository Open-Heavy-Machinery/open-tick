/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file trace.h
 * @brief CSV trace of a run, with columns registered by the application.
 *
 * One row per step, readable by any spreadsheet or by pandas. The
 * application declares its columns once, then writes each row value by
 * value, in column order:
 *
 *     static const OtkTraceColumn COLS[] = {
 *         { "t_s", OTK_COL_REAL, 3 },
 *         { "rpm", OTK_COL_REAL, 2 },
 *         { "state", OTK_COL_TEXT, 0 },
 *     };
 *     otk_trace_init(&tr, fp, COLS, 3);      // writes the header
 *     otk_trace_real(&tr, t_s);
 *     otk_trace_real(&tr, rpm);
 *     otk_trace_text(&tr, "RUNNING");
 *     otk_trace_end_row(&tr);
 *
 * A value of the wrong kind, a missing or extra column, or text that would
 * break the CSV (a comma, a quote, a line break) is an error. Errors are
 * sticky: the first one is kept, the writer keeps the file line-structured,
 * and otk_trace_ok() reports it. A trace that silently shifts its columns
 * would plot the wrong signal.
 *
 * The file belongs to the caller, who opens and closes it.
 */

#ifndef OPENTICK_TRACE_H
#define OPENTICK_TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef enum {
    OTK_COL_REAL = 0, /**< printed with a fixed number of decimals, like "%.2f" */
    OTK_COL_INT,      /**< signed integer, like "%d" */
    OTK_COL_UINT,     /**< unsigned integer, like "%u" */
    OTK_COL_TEXT      /**< text without commas, quotes or line breaks */
} OtkColumnKind;

typedef struct {
    const char *name;
    OtkColumnKind kind;
    int decimals; /**< OTK_COL_REAL only, 0..17 */
} OtkTraceColumn;

typedef struct {
    FILE *fp;
    const OtkTraceColumn *cols;
    size_t n_cols;
    size_t col;      /**< next column in the current row */
    uint64_t rows;   /**< rows completed */
    bool error;
    char error_msg[128];
} OtkTrace;

/**
 * @brief Checks the columns and writes the header line.
 * @return false if a column is malformed (no name, a comma in it, an
 *         unknown kind, decimals out of range) or the header cannot be written
 */
bool otk_trace_init(OtkTrace *t, FILE *fp, const OtkTraceColumn *cols, size_t n_cols);

/** @name Values of the current row, in column order
 * @{ */
void otk_trace_real(OtkTrace *t, double v);
void otk_trace_int(OtkTrace *t, long long v);
void otk_trace_uint(OtkTrace *t, unsigned long long v);
void otk_trace_text(OtkTrace *t, const char *s);
void otk_trace_empty(OtkTrace *t); /**< a column with no value in this row (any kind) */
/** @} */

/**
 * @brief Ends the row. Missing columns are left empty and reported as an error.
 * @return otk_trace_ok()
 */
bool otk_trace_end_row(OtkTrace *t);

/** @brief False after the first error. */
bool otk_trace_ok(const OtkTrace *t);

/** @brief The first error, or "" if there is none. */
const char *otk_trace_error(const OtkTrace *t);

#endif /* OPENTICK_TRACE_H */
