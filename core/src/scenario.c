/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file scenario.c
 * @brief Scenario CSV parser. See scenario.h.
 *
 * Extracted from open-crank v0.1 (sim/runtime/src/scenario.c). Changes:
 * signal names come from a table the application registers instead of a
 * fixed enum; times are nanoseconds instead of milliseconds.
 */

#include <opentick/scenario.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE 256U
#define MAX_FILE_BYTES 262144U

static bool fail(char *err, size_t err_len, unsigned line, const char *fmt, ...) {
    if (err != NULL && err_len > 0U) {
        int used = (line > 0U) ? snprintf(err, err_len, "line %u: ", line) : 0;
        if (used < 0) {
            used = 0;
        }
        if ((size_t)used < err_len) {
            va_list ap;
            va_start(ap, fmt);
            (void)vsnprintf(err + used, err_len - (size_t)used, fmt, ap);
            va_end(ap);
        }
    }
    return false;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    size_t n = strlen(s);
    while (n > 0U && (s[n - 1U] == ' ' || s[n - 1U] == '\t' || s[n - 1U] == '\r')) {
        s[--n] = '\0';
    }
    return s;
}

static bool parse_number(const char *s, double *out) {
    char *end = NULL;
    if (*s == '\0') {
        return false;
    }
    *out = strtod(s, &end);
    return end != NULL && *end == '\0' && isfinite(*out);
}

static bool parse(const char *text, const OtkSignalTable *signals, OtkScenario *s, char *err, size_t err_len) {
    char table_err[160];
    if (!otk_signal_table_check(signals, table_err, sizeof(table_err))) {
        return fail(err, err_len, 0U, "signal table: %s", table_err);
    }

    bool header_seen = false;
    unsigned line_no = 0U;
    const char *p = text;

    while (*p != '\0') {
        line_no++;
        const char *eol = strchr(p, '\n');
        const size_t len = (eol != NULL) ? (size_t)(eol - p) : strlen(p);
        if (len >= MAX_LINE) {
            return fail(err, err_len, line_no, "line longer than %u characters", MAX_LINE - 1U);
        }
        char buf[MAX_LINE];
        memcpy(buf, p, len);
        buf[len] = '\0';
        p = (eol != NULL) ? eol + 1 : p + len;

        char *line = trim(buf);
        if (*line == '\0' || *line == '#') {
            continue;
        }
        if (!header_seen) {
            if (strcmp(line, "time_s,signal,value") != 0) {
                return fail(err, err_len, line_no, "expected the header 'time_s,signal,value'");
            }
            header_seen = true;
            continue;
        }

        char *f1 = line;
        char *c1 = strchr(f1, ',');
        char *c2 = (c1 != NULL) ? strchr(c1 + 1, ',') : NULL;
        if (c1 == NULL || c2 == NULL || strchr(c2 + 1, ',') != NULL) {
            return fail(err, err_len, line_no, "expected three fields: time_s,signal,value");
        }
        *c1 = '\0';
        *c2 = '\0';
        char *f_time = trim(f1);
        char *f_signal = trim(c1 + 1);
        char *f_value = trim(c2 + 1);

        double t_s = 0.0;
        if (!parse_number(f_time, &t_s) || t_s < 0.0 || t_s > OTK_SCENARIO_MAX_TIME_S) {
            return fail(err, err_len, line_no, "bad time '%s'", f_time);
        }
        OtkSignalId id;
        if (!otk_signal_find(signals, f_signal, &id)) {
            return fail(err, err_len, line_no, "unknown signal '%s'", f_signal);
        }
        double value = 0.0;
        if (!parse_number(f_value, &value)) {
            return fail(err, err_len, line_no, "bad value '%s'", f_value);
        }

        /* Below OTK_SCENARIO_MAX_TIME_S the product stays under 2^53, so the
         * conversion is exact and rounding to the nearest nanosecond holds. */
        const OtkNs t = (OtkNs)(t_s * 1e9 + 0.5);
        if (s->n > 0U && t < s->events[s->n - 1U].t) {
            return fail(err, err_len, line_no, "time %.9g s is earlier than the previous event", t_s);
        }
        if (s->has_end) {
            return fail(err, err_len, line_no, "event after 'end'");
        }
        if (s->n >= OTK_SCENARIO_MAX_EVENTS) {
            return fail(err, err_len, line_no, "more than %u events", OTK_SCENARIO_MAX_EVENTS);
        }
        s->events[s->n].t = t;
        s->events[s->n].signal = id;
        s->events[s->n].value = value;
        s->n++;
        if (id == OTK_SIGNAL_END) {
            s->has_end = true;
            s->end = t;
        }
    }

    if (!header_seen) {
        return fail(err, err_len, 0U, "empty scenario: no 'time_s,signal,value' header");
    }
    return true;
}

bool otk_scenario_parse(const char *text, const OtkSignalTable *signals, OtkScenario *s, char *err, size_t err_len) {
    memset(s, 0, sizeof(*s));
    if (!parse(text, signals, s, err, err_len)) {
        s->n = 0U;
        s->has_end = false;
        s->end = 0U;
        return false;
    }
    return true;
}

bool otk_scenario_load(const char *path, const OtkSignalTable *signals, OtkScenario *s, char *err, size_t err_len) {
    memset(s, 0, sizeof(*s));
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return fail(err, err_len, 0U, "%s: cannot open", path);
    }
    static char text[MAX_FILE_BYTES + 1U];
    const size_t n = fread(text, 1U, MAX_FILE_BYTES + 1U, fp);
    const bool read_error = ferror(fp) != 0;
    fclose(fp);
    if (read_error) {
        return fail(err, err_len, 0U, "%s: read error", path);
    }
    if (n > MAX_FILE_BYTES) {
        return fail(err, err_len, 0U, "%s: larger than %u bytes", path, MAX_FILE_BYTES);
    }
    text[n] = '\0';
    if (memchr(text, '\0', n) != NULL) {
        return fail(err, err_len, 0U, "%s: not a text file (contains a NUL byte)", path);
    }

    char inner[200];
    if (!otk_scenario_parse(text, signals, s, inner, sizeof(inner))) {
        return fail(err, err_len, 0U, "%s: %s", path, inner);
    }
    return true;
}
