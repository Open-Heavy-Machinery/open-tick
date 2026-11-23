/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file oracle.c
 * @brief Acceptance oracle. See oracle.h and docs/criteria-format.md.
 *
 * Only the C standard library and open-tick's core types: this file must not
 * know about any application or protocol.
 */

#include <opentick/oracle.h>

#include <ctype.h>
#include <float.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define HEADER "id,from,until,tolerance_ms,condition,text"
#define MAX_TOLERANCE_MS 3600000UL
#define MAX_OFFSET_MS 4000000000LL
#define NS_PER_MS ((int64_t)OTK_NS_PER_MS)

typedef struct {
    OtkOracle *o;
    const OtkScenario *scenario;
    const OtkSignalTable *signals;
    size_t lineno;
    bool header_seen;
    char *err;
    size_t err_len;
} Parser;

static bool fail(Parser *p, const char *fmt, ...) {
    if (p->err != NULL && p->err_len > 0U) {
        int used = 0;
        if (p->lineno > 0U) {
            used = snprintf(p->err, p->err_len, "line %lu: ", (unsigned long)p->lineno);
        }
        if (used >= 0 && (size_t)used < p->err_len) {
            va_list ap;
            va_start(ap, fmt);
            (void)vsnprintf(p->err + used, p->err_len - (size_t)used, fmt, ap);
            va_end(ap);
        }
    }
    return false;
}

/* Strips leading and trailing white space in place. */
static char *trim(char *s) {
    while (*s != '\0' && isspace((unsigned char)*s)) {
        s++;
    }
    size_t n = strlen(s);
    while (n > 0U && isspace((unsigned char)s[n - 1U])) {
        s[--n] = '\0';
    }
    return s;
}

/* A finite number taking the whole string. */
static bool parse_number(const char *s, double *out) {
    if (*s == '\0') {
        return false;
    }
    char *end = NULL;
    const double v = strtod(s, &end);
    if (end == s || *end != '\0' || !(v >= -DBL_MAX && v <= DBL_MAX)) {
        return false;
    }
    *out = v;
    return true;
}

static bool all_digits(const char *s) {
    if (*s == '\0') {
        return false;
    }
    for (; *s != '\0'; s++) {
        if (!isdigit((unsigned char)*s)) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------ references */

/*
 * `begin`, `end`, seconds, or `<signal>=<value>`, then an optional `+Nms` or
 * `-Nms`. Sets *event when the reference is a scenario event (`end` too) and
 * copies the reference without its offset to label.
 */
static bool parse_ref(Parser *p, const char *field, const char *what, int64_t *ns, bool *event, int64_t *event_ns,
                      int64_t *offset, char *label, size_t label_len) {
    char buf[OTK_ORACLE_MAX_LINE];
    (void)snprintf(buf, sizeof(buf), "%s", field);
    char *base = trim(buf);
    *offset = 0;

    const size_t n = strlen(base);
    if (n > 2U && base[n - 2U] == 'm' && base[n - 1U] == 's') {
        size_t i = n - 2U;
        while (i > 0U && isdigit((unsigned char)base[i - 1U])) {
            i--;
        }
        if (i < n - 2U && i > 0U && (base[i - 1U] == '+' || base[i - 1U] == '-')) {
            const char sign = base[i - 1U];
            base[n - 2U] = '\0';
            if (strlen(&base[i]) > 10U) {
                return fail(p, "offset too large in %s '%s'", what, field);
            }
            const long long v = strtoll(&base[i], NULL, 10);
            if (v > MAX_OFFSET_MS) {
                return fail(p, "offset too large in %s '%s'", what, field);
            }
            *offset = ((sign == '-') ? -(int64_t)v : (int64_t)v) * NS_PER_MS;
            base[i - 1U] = '\0';
            base = trim(base);
        }
    }
    if (*base == '\0') {
        return fail(p, "empty %s", what);
    }
    (void)snprintf(label, label_len, "%s", base);

    int64_t t = 0;
    *event = false;
    char *eq = strchr(base, '=');
    if (strcmp(base, "begin") == 0) {
        t = 0;
    } else if (strcmp(base, OTK_SIGNAL_END_NAME) == 0) {
        if (!p->scenario->has_end) {
            return fail(p, "%s refers to 'end', but the scenario has no end event", what);
        }
        t = (int64_t)p->scenario->end;
        *event = true;
    } else if (eq != NULL) {
        *eq = '\0';
        const char *name = trim(base);
        const char *value_text = trim(eq + 1);
        OtkSignalId id;
        if (!otk_signal_find(p->signals, name, &id) || id == OTK_SIGNAL_END) {
            return fail(p, "unknown signal '%s' in %s '%s'", name, what, field);
        }
        double value;
        if (!parse_number(value_text, &value)) {
            return fail(p, "bad value '%s' in %s '%s'", value_text, what, field);
        }
        bool found = false;
        for (size_t i = 0; i < p->scenario->n; i++) {
            const OtkEvent *e = &p->scenario->events[i];
            if (e->signal == id && e->value == value) {
                t = (int64_t)e->t;
                found = true;
                break;
            }
        }
        if (!found) {
            return fail(p, "no event in the scenario sets '%s' to %s (%s '%s')", name, value_text, what, field);
        }
        *event = true;
    } else {
        double s;
        if (!parse_number(base, &s) || s < 0.0 || s > OTK_SCENARIO_MAX_TIME_S) {
            return fail(p, "bad reference '%s' in %s", field, what);
        }
        t = (int64_t)(s * 1e9 + 0.5);
    }
    *event_ns = t;
    *ns = t + *offset;
    if (*ns < 0) {
        return fail(p, "%s '%s' is before t = 0", what, field);
    }
    return true;
}

/* ------------------------------------------------------------ conditions */

static bool parse_term(Parser *p, OtkOracleCriterion *c, char *text) {
    char *s = trim(text);
    if (c->n_terms >= OTK_ORACLE_MAX_TERMS) {
        return fail(p, "more than %u terms in the condition", (unsigned)OTK_ORACLE_MAX_TERMS);
    }
    char *op = strpbrk(s, "<>=!");
    if (op == NULL) {
        return fail(p, "no operator in term '%s'", s);
    }
    OtkOracleOp code;
    size_t op_len = 1U;
    const bool eq_next = op[1] == '=';
    switch (*op) {
    case '<':
        code = eq_next ? OTK_ORACLE_LE : OTK_ORACLE_LT;
        break;
    case '>':
        code = eq_next ? OTK_ORACLE_GE : OTK_ORACLE_GT;
        break;
    case '=':
        code = OTK_ORACLE_EQ;
        break;
    default: /* '!' */
        code = OTK_ORACLE_NE;
        break;
    }
    if (eq_next) {
        op_len = 2U;
    } else if (*op == '=' || *op == '!') {
        return fail(p, "bad operator in term '%s'", s);
    } else {
        /* '<' or '>' alone */
    }
    char *rhs_text = trim(op + op_len);
    if (*rhs_text != '\0' && strchr("<>=!", *rhs_text) != NULL) {
        return fail(p, "bad operator in term '%s'", s);
    }
    double rhs;
    if (!parse_number(rhs_text, &rhs)) {
        return fail(p, "bad number '%s' in term '%s'", rhs_text, s);
    }
    *op = '\0';
    const char *name = trim(s);
    if (*name == '\0') {
        return fail(p, "no observable before the operator");
    }
    const OtkOracleTable *table = p->o->table;
    size_t index = table->count;
    for (size_t i = 0; i < table->count; i++) {
        if (strcmp(name, table->observables[i].name) == 0) {
            index = i;
            break;
        }
    }
    if (index == table->count) {
        return fail(p, "unknown observable '%s'", name);
    }
    c->terms[c->n_terms].observable = index;
    c->terms[c->n_terms].op = code;
    c->terms[c->n_terms].rhs = rhs;
    c->n_terms++;
    return true;
}

static bool parse_conjunction(Parser *p, OtkOracleCriterion *c, char *text) {
    if (*trim(text) == '\0') {
        return fail(p, "empty condition");
    }
    char *s = text;
    for (;;) {
        char *amp = strstr(s, "&&");
        if (amp != NULL) {
            *amp = '\0';
        }
        if (*trim(s) == '\0') {
            return fail(p, "empty term in the condition");
        }
        if (!parse_term(p, c, s)) {
            return false;
        }
        if (amp == NULL) {
            return true;
        }
        s = amp + 2;
    }
}

static bool parse_condition(Parser *p, OtkOracleCriterion *c, char *text) {
    char *arrow = strstr(text, "->");
    if (arrow != NULL) {
        if (strstr(arrow + 2, "->") != NULL) {
            return fail(p, "more than one '->' in the condition");
        }
        *arrow = '\0';
        if (!parse_conjunction(p, c, text)) {
            return false;
        }
        c->n_antecedent = c->n_terms;
        return parse_conjunction(p, c, arrow + 2);
    }
    c->n_antecedent = 0U;
    return parse_conjunction(p, c, text);
}

/* ------------------------------------------------------------ lines */

static bool valid_id(const char *s) {
    if (*s == '\0') {
        return false;
    }
    for (; *s != '\0'; s++) {
        if (!isalnum((unsigned char)*s) && *s != '_' && *s != '.' && *s != '-') {
            return false;
        }
    }
    return true;
}

static bool parse_criterion(Parser *p, char *line) {
    OtkOracle *o = p->o;
    char *field[6];
    field[0] = line;
    for (size_t i = 1; i < 6U; i++) {
        char *comma = strchr(field[i - 1U], ',');
        if (comma == NULL) {
            return fail(p, "expected six fields: " HEADER);
        }
        *comma = '\0';
        field[i] = comma + 1;
    }
    if (o->n >= OTK_ORACLE_MAX_CRITERIA) {
        return fail(p, "more than %u criteria", (unsigned)OTK_ORACLE_MAX_CRITERIA);
    }
    OtkOracleCriterion *c = &o->criteria[o->n];
    memset(c, 0, sizeof(*c));
    c->true_since_ns = -1;

    const char *id = trim(field[0]);
    if (!valid_id(id) || strlen(id) >= OTK_ORACLE_MAX_ID) {
        return fail(p, "bad id '%s' (letters, digits, '_', '.', '-'; at most %u characters)", id,
                    (unsigned)(OTK_ORACLE_MAX_ID - 1U));
    }
    for (size_t i = 0; i < o->n; i++) {
        if (strcmp(o->criteria[i].id, id) == 0) {
            return fail(p, "duplicate id '%s'", id);
        }
    }
    (void)snprintf(c->id, sizeof(c->id), "%s", id);

    const char *text = trim(field[5]);
    if (*text == '\0') {
        return fail(p, "empty text");
    }
    if (strlen(text) >= OTK_ORACLE_MAX_TEXT) {
        return fail(p, "text longer than %u characters", (unsigned)(OTK_ORACLE_MAX_TEXT - 1U));
    }
    (void)snprintf(c->text, sizeof(c->text), "%s", text);

    bool from_event = false;
    bool until_event = false;
    int64_t from_offset = 0;
    int64_t until_offset = 0;
    int64_t until_event_ns = 0;
    char until_label[OTK_ORACLE_MAX_LABEL];
    if (!parse_ref(p, field[1], "from", &c->from_ns, &from_event, &c->event_ns, &from_offset, c->event,
                   sizeof(c->event)) ||
        !parse_ref(p, field[2], "until", &c->until_ns, &until_event, &until_event_ns, &until_offset, until_label,
                   sizeof(until_label))) {
        return false;
    }
    c->from_event = from_event && from_offset > 0;

    const char *tol = trim(field[3]);
    if (!all_digits(tol) || strlen(tol) > 7U || strtoul(tol, NULL, 10) > MAX_TOLERANCE_MS) {
        return fail(p, "bad tolerance_ms '%s' (whole milliseconds, at most %lu)", tol, MAX_TOLERANCE_MS);
    }
    c->tolerance_ns = (int64_t)strtoul(tol, NULL, 10) * NS_PER_MS;

    if (!parse_condition(p, c, field[4])) {
        return false;
    }

    if (c->from_ns > c->until_ns) {
        return fail(p, "empty window: from %.3f s is after until %.3f s", (double)c->from_ns / 1e9,
                    (double)c->until_ns / 1e9);
    }
    if (p->scenario->has_end && c->until_ns > (int64_t)p->scenario->end) {
        return fail(p, "until %.3f s is after the scenario's end (%.3f s)", (double)c->until_ns / 1e9,
                    (double)p->scenario->end / 1e9);
    }
    o->n++;
    return true;
}

static bool parse_line(Parser *p, char *raw) {
    char *line = trim(raw);
    if (*line == '\0' || *line == '#') {
        return true;
    }
    if (!p->header_seen) {
        if (strcmp(line, HEADER) != 0) {
            return fail(p, "expected the header '" HEADER "'");
        }
        p->header_seen = true;
        return true;
    }
    return parse_criterion(p, line);
}

static bool begin(Parser *p, OtkOracle *o, const OtkOracleTable *table, const OtkScenario *scenario,
                  const OtkSignalTable *signals, OtkNs step, char *err, size_t err_len) {
    memset(o, 0, sizeof(*o));
    p->o = o;
    p->scenario = scenario;
    p->signals = signals;
    p->lineno = 0U;
    p->header_seen = false;
    p->err = err;
    p->err_len = err_len;
    if (err != NULL && err_len > 0U) {
        err[0] = '\0';
    }
    if (table == NULL || table->observables == NULL || table->count == 0U ||
        table->count > OTK_ORACLE_MAX_OBSERVABLES || table->decode == NULL) {
        return fail(p, "bad observable table (1 to %u observables and a decoder)",
                    (unsigned)OTK_ORACLE_MAX_OBSERVABLES);
    }
    for (size_t i = 0; i < table->count; i++) {
        if (table->observables[i].name == NULL || table->observables[i].name[0] == '\0') {
            return fail(p, "observable %lu has no name", (unsigned long)i);
        }
    }
    if (scenario == NULL || signals == NULL) {
        return fail(p, "no scenario or signal table");
    }
    if (step == 0U || step > (OtkNs)INT64_MAX) {
        return fail(p, "step must be at least 1 ns");
    }
    o->table = table;
    o->step = step;
    return true;
}

static bool finish(Parser *p, bool ok) {
    if (ok && !p->header_seen) {
        p->lineno = 0U;
        ok = fail(p, "no header '" HEADER "'");
    } else if (ok && p->o->n == 0U) {
        p->lineno = 0U;
        ok = fail(p, "no criteria");
    } else {
        /* ok, or the error is already written */
    }
    if (!ok) {
        p->o->n = 0U;
    }
    return ok;
}

bool otk_oracle_parse(OtkOracle *o, const char *text, const OtkOracleTable *table, const OtkScenario *scenario,
                      const OtkSignalTable *signals, OtkNs step, char *err, size_t err_len) {
    Parser p;
    if (!begin(&p, o, table, scenario, signals, step, err, err_len)) {
        o->n = 0U;
        return false;
    }
    bool ok = true;
    const char *s = text;
    while (ok && *s != '\0') {
        p.lineno++;
        const char *nl = strchr(s, '\n');
        const size_t len = (nl != NULL) ? (size_t)(nl - s) : strlen(s);
        char line[OTK_ORACLE_MAX_LINE];
        if (len >= sizeof(line)) {
            ok = fail(&p, "longer than %u characters", (unsigned)(OTK_ORACLE_MAX_LINE - 1U));
            break;
        }
        memcpy(line, s, len);
        line[len] = '\0';
        ok = parse_line(&p, line);
        s = (nl != NULL) ? nl + 1 : s + len;
    }
    return finish(&p, ok);
}

bool otk_oracle_load(OtkOracle *o, const char *path, const OtkOracleTable *table, const OtkScenario *scenario,
                     const OtkSignalTable *signals, OtkNs step, char *err, size_t err_len) {
    Parser p;
    if (!begin(&p, o, table, scenario, signals, step, err, err_len)) {
        o->n = 0U;
        return false;
    }
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        (void)fail(&p, "cannot open '%s'", path);
        o->n = 0U;
        return false;
    }
    bool ok = true;
    char line[OTK_ORACLE_MAX_LINE + 1U];
    while (ok && fgets(line, (int)sizeof(line), fp) != NULL) {
        p.lineno++;
        const size_t len = strlen(line);
        if (len > 0U && line[len - 1U] != '\n' && !feof(fp)) {
            ok = fail(&p, "longer than %u characters", (unsigned)(OTK_ORACLE_MAX_LINE - 1U));
            break;
        }
        ok = parse_line(&p, line);
    }
    if (ok && ferror(fp)) {
        p.lineno = 0U;
        ok = fail(&p, "cannot read '%s'", path);
    }
    fclose(fp);
    return finish(&p, ok);
}

/* ------------------------------------------------------------ grading */

void otk_oracle_frame(OtkOracle *o, const OtkCanFrame *f, OtkNs t) {
    if (o->table == NULL) {
        return;
    }
    OtkOracleUpdate up[OTK_ORACLE_MAX_OBSERVABLES];
    size_t n = o->table->decode(o->table->ctx, f, up, OTK_ORACLE_MAX_OBSERVABLES);
    if (n > OTK_ORACLE_MAX_OBSERVABLES) {
        n = OTK_ORACLE_MAX_OBSERVABLES;
    }
    for (size_t i = 0; i < n; i++) {
        const size_t k = up[i].observable;
        if (k < o->table->count) {
            o->last[k] = up[i].value;
            o->seen[k] = true;
            o->last_ns[k] = t;
        }
    }
}

static bool term_holds(const OtkOracle *o, const OtkOracleTerm *t) {
    const OtkOracleValue v = o->now[t->observable];
    if (v.gap) {
        return false;
    }
    switch (t->op) {
    case OTK_ORACLE_EQ:
        return v.value == t->rhs;
    case OTK_ORACLE_NE:
        return v.value != t->rhs;
    case OTK_ORACLE_LT:
        return v.value < t->rhs;
    case OTK_ORACLE_LE:
        return v.value <= t->rhs;
    case OTK_ORACLE_GT:
        return v.value > t->rhs;
    case OTK_ORACLE_GE:
        return v.value >= t->rhs;
    default:
        return false;
    }
}

static bool all_hold(const OtkOracle *o, const OtkOracleTerm *terms, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (!term_holds(o, &terms[i])) {
            return false;
        }
    }
    return true;
}

static bool condition_holds(const OtkOracle *o, const OtkOracleCriterion *c) {
    if (c->n_antecedent == 0U) {
        return all_hold(o, c->terms, c->n_terms);
    }
    return !all_hold(o, c->terms, c->n_antecedent) ||
           all_hold(o, &c->terms[c->n_antecedent], c->n_terms - c->n_antecedent);
}

void otk_oracle_step(OtkOracle *o, OtkNs t) {
    if (o->table == NULL) {
        return;
    }
    const int64_t now_ns = (int64_t)t;
    for (size_t k = 0; k < o->table->count; k++) {
        const int64_t age = now_ns - (int64_t)o->last_ns[k];
        const bool fresh = o->seen[k] && age <= (int64_t)o->table->observables[k].stale;
        o->now[k] = fresh ? o->last[k] : o->table->observables[k].fallback;
    }

    for (size_t i = 0; i < o->n; i++) {
        OtkOracleCriterion *c = &o->criteria[i];
        const bool ok = condition_holds(o, c);

        if (c->from_event && !c->reaction_done && now_ns >= c->event_ns) {
            if (!ok) {
                c->true_since_ns = -1;
            } else if (c->true_since_ns < 0) {
                c->true_since_ns = now_ns;
            } else {
                /* still holding */
            }
            if (now_ns >= c->from_ns) {
                c->reaction_done = true;
                c->reaction_holding = ok;
            }
        }

        if (now_ns < c->from_ns || now_ns > c->until_ns) {
            continue;
        }
        c->evaluated = true;
        c->last_ns = now_ns;
        if (ok) {
            c->false_run = false;
            continue;
        }
        if (!c->false_run) {
            c->false_run = true;
            c->false_since_ns = now_ns;
            memcpy(c->false_values, o->now, sizeof(c->false_values));
        }
        if (!c->failed && (now_ns - c->false_since_ns + (int64_t)o->step) > c->tolerance_ns) {
            c->failed = true;
            c->fail_ns = c->false_since_ns;
            memcpy(c->fail_values, c->false_values, sizeof(c->fail_values));
        }
    }
}

/* The window was evaluated up to its end (the last step inside it is within one step of `until`). */
static bool covered(const OtkOracle *o, const OtkOracleCriterion *c) {
    return c->evaluated && c->last_ns + (int64_t)o->step > c->until_ns;
}

static bool passed(const OtkOracle *o, const OtkOracleCriterion *c) {
    return covered(o, c) && !c->failed;
}

static void print_values(const OtkOracle *o, const OtkOracleCriterion *c, FILE *fp) {
    bool printed[OTK_ORACLE_MAX_OBSERVABLES] = { false };
    const char *sep = " ";
    for (size_t i = 0; i < c->n_terms; i++) {
        const size_t k = c->terms[i].observable;
        if (printed[k]) {
            continue;
        }
        printed[k] = true;
        if (c->fail_values[k].gap) {
            fprintf(fp, "%s%s = gap", sep, o->table->observables[k].name);
        } else {
            fprintf(fp, "%s%s = %g", sep, o->table->observables[k].name, c->fail_values[k].value);
        }
        sep = ", ";
    }
}

/* Whole milliseconds when exact; a reaction that is not a whole number of ms keeps its fraction. */
static void print_reaction(int64_t ns, const char *event, FILE *fp) {
    if (ns % NS_PER_MS == 0) {
        fprintf(fp, "  reaction: %lld ms after %s", (long long)(ns / NS_PER_MS), event);
    } else {
        fprintf(fp, "  reaction: %.6f ms after %s", (double)ns / 1e6, event);
    }
}

bool otk_oracle_report(const OtkOracle *o, FILE *fp) {
    bool all = true;
    for (size_t i = 0; i < o->n; i++) {
        const OtkOracleCriterion *c = &o->criteria[i];
        const bool ok = passed(o, c);
        all = all && ok;
        fprintf(fp, "%s  %s  %s  [%.2f s .. %.2f s]", ok ? "PASS" : "FAIL", c->id, c->text,
                (double)c->from_ns / 1e9, (double)c->until_ns / 1e9);
        if (c->from_event) {
            if (!c->reaction_done) {
                fprintf(fp, "  reaction: not measured");
            } else if (c->reaction_holding) {
                print_reaction(c->true_since_ns - c->event_ns, c->event, fp);
            } else {
                fprintf(fp, "  reaction: not holding at %.2f s", (double)c->from_ns / 1e9);
            }
        }
        if (c->failed) {
            fprintf(fp, "  first violation at %.2f s:", (double)c->fail_ns / 1e9);
            print_values(o, c, fp);
        } else if (!c->evaluated) {
            fprintf(fp, "  not evaluated: no step fell inside the window");
        } else if (!covered(o, c)) {
            fprintf(fp, "  not evaluated to the end: the last step was at %.2f s", (double)c->last_ns / 1e9);
        } else {
            /* passed */
        }
        fputc('\n', fp);
    }
    return all;
}

size_t otk_oracle_count(const OtkOracle *o) {
    return o->n;
}

size_t otk_oracle_failures(const OtkOracle *o) {
    size_t n = 0U;
    for (size_t i = 0; i < o->n; i++) {
        if (!passed(o, &o->criteria[i])) {
            n++;
        }
    }
    return n;
}

bool otk_oracle_failed(const OtkOracle *o, const char *id) {
    for (size_t i = 0; i < o->n; i++) {
        if (strcmp(o->criteria[i].id, id) == 0) {
            return !passed(o, &o->criteria[i]);
        }
    }
    return false;
}
