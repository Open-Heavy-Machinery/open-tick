/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file signals.c
 * @brief Signal tables. See signals.h.
 */

#include <opentick/signals.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static bool fail(char *err, size_t err_len, const char *fmt, ...) {
    if (err != NULL && err_len > 0U) {
        va_list ap;
        va_start(ap, fmt);
        (void)vsnprintf(err, err_len, fmt, ap);
        va_end(ap);
    }
    return false;
}

static bool name_char_ok(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
           c == '-';
}

bool otk_signal_table_check(const OtkSignalTable *t, char *err, size_t err_len) {
    if (t == NULL || (t->count > 0U && t->names == NULL)) {
        return fail(err, err_len, "no signal table");
    }
    if (t->count >= (size_t)OTK_SIGNAL_END) {
        return fail(err, err_len, "too many signals (%zu)", t->count);
    }
    for (size_t i = 0; i < t->count; i++) {
        const char *name = t->names[i];
        if (name == NULL || name[0] == '\0') {
            return fail(err, err_len, "signal %zu: no name", i);
        }
        for (const char *c = name; *c != '\0'; c++) {
            if (!name_char_ok(*c)) {
                return fail(err, err_len, "signal %zu: '%s' has a character other than letters, digits, '_', '.', '-'",
                            i, name);
            }
        }
        if (strcmp(name, OTK_SIGNAL_END_NAME) == 0) {
            return fail(err, err_len, "signal %zu: '%s' is reserved", i, name);
        }
        for (size_t j = 0; j < i; j++) {
            if (strcmp(name, t->names[j]) == 0) {
                return fail(err, err_len, "signal %zu: duplicate name '%s' (also signal %zu)", i, name, j);
            }
        }
    }
    return true;
}

bool otk_signal_find(const OtkSignalTable *t, const char *name, OtkSignalId *id) {
    if (strcmp(name, OTK_SIGNAL_END_NAME) == 0) {
        *id = OTK_SIGNAL_END;
        return true;
    }
    for (size_t i = 0; i < t->count; i++) {
        if (strcmp(name, t->names[i]) == 0) {
            *id = (OtkSignalId)i;
            return true;
        }
    }
    return false;
}

const char *otk_signal_name(const OtkSignalTable *t, OtkSignalId id) {
    if (id == OTK_SIGNAL_END) {
        return OTK_SIGNAL_END_NAME;
    }
    return ((size_t)id < t->count) ? t->names[id] : "?";
}
