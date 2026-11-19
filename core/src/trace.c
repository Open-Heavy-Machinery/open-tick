/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file trace.c
 * @brief CSV trace writer. See trace.h.
 *
 * Extracted from open-crank v0.1 (sim/runtime/src/trace.c), where the
 * columns were hard-coded. The number formats are the same printf formats,
 * so a trace written through this writer is byte-for-byte the one the
 * hard-coded version wrote.
 */

#include <opentick/trace.h>

#include <stdarg.h>
#include <string.h>

#define MAX_DECIMALS 17

static void set_error(OtkTrace *t, const char *fmt, ...) {
    if (t->error) {
        return; /* keep the first one */
    }
    t->error = true;
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(t->error_msg, sizeof(t->error_msg), fmt, ap);
    va_end(ap);
}

static bool csv_safe(const char *s) {
    return strpbrk(s, ",\"\r\n") == NULL;
}

static void put(OtkTrace *t, int written) {
    if (written < 0) {
        set_error(t, "row %llu: write error", (unsigned long long)t->rows + 1U);
    }
}

bool otk_trace_init(OtkTrace *t, FILE *fp, const OtkTraceColumn *cols, size_t n_cols) {
    memset(t, 0, sizeof(*t));
    t->fp = fp;
    t->cols = cols;
    t->n_cols = n_cols;

    if (fp == NULL || cols == NULL || n_cols == 0U) {
        set_error(t, "no file or no columns");
        return false;
    }
    for (size_t i = 0; i < n_cols; i++) {
        const OtkTraceColumn *c = &cols[i];
        if (c->name == NULL || c->name[0] == '\0' || !csv_safe(c->name)) {
            set_error(t, "column %zu: missing name or a comma, quote or line break in it", i);
            return false;
        }
        if (c->kind != OTK_COL_REAL && c->kind != OTK_COL_INT && c->kind != OTK_COL_UINT && c->kind != OTK_COL_TEXT) {
            set_error(t, "column '%s': unknown kind", c->name);
            return false;
        }
        if (c->kind == OTK_COL_REAL && (c->decimals < 0 || c->decimals > MAX_DECIMALS)) {
            set_error(t, "column '%s': decimals must be 0..%d", c->name, MAX_DECIMALS);
            return false;
        }
    }

    for (size_t i = 0; i < n_cols; i++) {
        put(t, fprintf(fp, "%s%s", (i > 0U) ? "," : "", cols[i].name));
    }
    put(t, fputc('\n', fp) == EOF ? -1 : 1);
    return !t->error;
}

/* Starts the next column. Returns the column, or NULL if the row is already full. */
static const OtkTraceColumn *next_col(OtkTrace *t) {
    if (t->col >= t->n_cols) {
        set_error(t, "row %llu: more than %zu values", (unsigned long long)t->rows + 1U, t->n_cols);
        return NULL;
    }
    if (t->col > 0U) {
        put(t, fputc(',', t->fp) == EOF ? -1 : 1);
    }
    return &t->cols[t->col++];
}

static void check_kind(OtkTrace *t, const OtkTraceColumn *c, OtkColumnKind kind) {
    if (c->kind != kind) {
        set_error(t, "row %llu: column '%s' given a value of the wrong kind", (unsigned long long)t->rows + 1U,
                  c->name);
    }
}

void otk_trace_real(OtkTrace *t, double v) {
    const OtkTraceColumn *c = next_col(t);
    if (c == NULL) {
        return;
    }
    check_kind(t, c, OTK_COL_REAL);
    const int decimals = (c->kind == OTK_COL_REAL) ? c->decimals : 6;
    put(t, fprintf(t->fp, "%.*f", decimals, v));
}

void otk_trace_int(OtkTrace *t, long long v) {
    const OtkTraceColumn *c = next_col(t);
    if (c == NULL) {
        return;
    }
    check_kind(t, c, OTK_COL_INT);
    put(t, fprintf(t->fp, "%lld", v));
}

void otk_trace_uint(OtkTrace *t, unsigned long long v) {
    const OtkTraceColumn *c = next_col(t);
    if (c == NULL) {
        return;
    }
    check_kind(t, c, OTK_COL_UINT);
    put(t, fprintf(t->fp, "%llu", v));
}

void otk_trace_text(OtkTrace *t, const char *s) {
    const OtkTraceColumn *c = next_col(t);
    if (c == NULL) {
        return;
    }
    check_kind(t, c, OTK_COL_TEXT);
    if (s == NULL || !csv_safe(s)) {
        set_error(t, "row %llu: column '%s': missing text, or a comma, quote or line break in it",
                  (unsigned long long)t->rows + 1U, c->name);
        return; /* leave the field empty rather than break the row */
    }
    put(t, fputs(s, t->fp) == EOF ? -1 : 1);
}

void otk_trace_empty(OtkTrace *t) {
    (void)next_col(t);
}

bool otk_trace_end_row(OtkTrace *t) {
    if (t->col < t->n_cols) {
        set_error(t, "row %llu: column '%s' and later missing", (unsigned long long)t->rows + 1U,
                  t->cols[t->col].name);
        while (t->col < t->n_cols) {
            (void)next_col(t);
        }
    }
    put(t, fputc('\n', t->fp) == EOF ? -1 : 1);
    t->col = 0U;
    t->rows++;
    return !t->error;
}

bool otk_trace_ok(const OtkTrace *t) {
    return !t->error;
}

const char *otk_trace_error(const OtkTrace *t) {
    return t->error ? t->error_msg : "";
}
