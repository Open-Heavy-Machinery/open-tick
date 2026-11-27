/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file fidelity.c
 * @brief The fidelity contract's parser and checker (fidelity.h, §7).
 */

#include <opentick/fidelity.h>

#include <stdio.h>
#include <string.h>

static const char HEADER[] = "signal,direction,transport,max_rate_hz,resolution_ns,latency_ns,jitter_ns,logic_v";
static const char *const COLUMN_NAMES[OTK_FIDELITY_NUM_COLUMNS] = {
    "max_rate_hz", "resolution_ns", "latency_ns", "jitter_ns", "logic_v",
};
#define NUM_FIELDS 8u

const char *otk_fidelity_column_name(OtkFidelityColumn c) {
    return (unsigned)c < OTK_FIDELITY_NUM_COLUMNS ? COLUMN_NAMES[c] : "?";
}

/* ---------------------------------------------------------------- parsing */

typedef struct {
    const char *p;
    size_t n;
} Span;

static bool span_is(Span s, const char *lit) {
    const size_t n = strlen(lit);
    return s.n == n && memcmp(s.p, lit, n) == 0;
}

static Span trim(Span s) {
    while (s.n > 0u && s.p[0] == ' ') {
        s.p++;
        s.n--;
    }
    while (s.n > 0u && s.p[s.n - 1u] == ' ') {
        s.n--;
    }
    return s;
}

static bool fail(OtkFidelityError *e, unsigned line, const char *column, const char *what) {
    e->line = line;
    e->column = column;
    e->what = what;
    return false;
}

/* A whole number: 1 to 19 digits, no sign. */
static bool parse_u64(Span s, uint64_t *v) {
    if (s.n == 0u || s.n > 19u) {
        return false;
    }
    uint64_t x = 0u;
    for (size_t i = 0; i < s.n; i++) {
        if (s.p[i] < '0' || s.p[i] > '9') {
            return false;
        }
        x = x * 10u + (uint64_t)(s.p[i] - '0');
    }
    *v = x;
    return true;
}

/* Volts with up to three decimals, as millivolts: "3.3" -> 3300. */
static bool parse_mv(Span s, uint64_t *mv) {
    size_t dot = s.n;
    for (size_t i = 0; i < s.n; i++) {
        if (s.p[i] == '.') {
            dot = i;
            break;
        }
    }
    uint64_t whole = 0u;
    if (dot == 0u || dot > 9u || !parse_u64((Span){ s.p, dot }, &whole)) {
        return false;
    }
    uint64_t frac = 0u;
    if (dot < s.n) {
        const size_t nd = s.n - dot - 1u;
        if (nd == 0u || nd > 3u || !parse_u64((Span){ s.p + dot + 1u, nd }, &frac)) {
            return false;
        }
        for (size_t i = nd; i < 3u; i++) {
            frac *= 10u;
        }
    }
    *mv = whole * 1000u + frac;
    return true;
}

static bool parse_metadata(Span line, OtkFidelityFile *f, OtkFidelityError *e) {
    static const char PREFIX[] = "# otk-fidelity ";
    const size_t np = sizeof(PREFIX) - 1u;
    if (line.n < np || memcmp(line.p, PREFIX, np) != 0) {
        return fail(e, 1u, NULL, "the first line is not \"# otk-fidelity <version>; ...\"");
    }
    Span rest = { line.p + np, line.n - np };
    bool have_side = false;
    bool first = true;
    while (rest.n > 0u || first) {
        size_t cut = 0u;
        while (cut < rest.n && rest.p[cut] != ';') {
            cut++;
        }
        const Span item = trim((Span){ rest.p, cut });
        if (first) {
            uint64_t version = 0u;
            if (!parse_u64(item, &version) || version != OTK_FIDELITY_FORMAT_VERSION) {
                return fail(e, 1u, NULL, "not version 1 of the format");
            }
            first = false;
        } else if (item.n > 0u) {
            size_t eq = 0u;
            while (eq < item.n && item.p[eq] != '=') {
                eq++;
            }
            if (eq == item.n) {
                return fail(e, 1u, NULL, "a metadata item is not key=value");
            }
            const Span key = trim((Span){ item.p, eq });
            const Span val = trim((Span){ item.p + eq + 1u, item.n - eq - 1u });
            if (span_is(key, "side")) {
                if (span_is(val, "machine")) {
                    f->side = OTK_FIDELITY_SIDE_MACHINE;
                } else if (span_is(val, "bridge")) {
                    f->side = OTK_FIDELITY_SIDE_BRIDGE;
                } else {
                    return fail(e, 1u, "side", "side is neither machine nor bridge");
                }
                have_side = true;
            } else if (span_is(key, "lead_ns")) {
                if (!parse_u64(val, &f->lead_ns)) {
                    return fail(e, 1u, "lead_ns", "not a whole number");
                }
                f->has_lead = true;
            } else if (span_is(key, "dut_to_plant_delay_ns")) {
                if (!parse_u64(val, &f->dut_to_plant_delay_ns)) {
                    return fail(e, 1u, "dut_to_plant_delay_ns", "not a whole number");
                }
                f->has_delay = true;
            } else if (span_is(key, "link")) {
                if (val.n >= sizeof(f->link)) {
                    return fail(e, 1u, "link", "too long");
                }
                memcpy(f->link, val.p, val.n);
                f->link[val.n] = '\0';
            }
            /* other keys (source, ...) are for people */
        }
        if (cut == rest.n) {
            break;
        }
        rest.p += cut + 1u;
        rest.n -= cut + 1u;
    }
    if (!have_side) {
        return fail(e, 1u, "side", "no side=machine or side=bridge");
    }
    return true;
}

static bool parse_row(Span line, unsigned ln, OtkFidelityFile *f, OtkFidelityError *e) {
    static const char *const FIELD[NUM_FIELDS] = {
        "signal", "direction", "transport", "max_rate_hz", "resolution_ns", "latency_ns", "jitter_ns", "logic_v",
    };
    Span field[NUM_FIELDS];
    size_t nf = 0u, start = 0u;
    for (size_t i = 0; i <= line.n; i++) {
        if (i == line.n || line.p[i] == ',') {
            if (nf == NUM_FIELDS) {
                return fail(e, ln, NULL, "more than 8 fields");
            }
            field[nf++] = (Span){ line.p + start, i - start };
            start = i + 1u;
        }
    }
    if (nf != NUM_FIELDS) {
        return fail(e, ln, NULL, "fewer than 8 fields");
    }
    if (f->n == OTK_FIDELITY_MAX_SIGNALS) {
        return fail(e, ln, NULL, "too many signals");
    }
    OtkFidelitySignal *s = &f->sig[f->n];
    memset(s, 0, sizeof(*s));
    s->line = ln;
    if (field[0].n == 0u || field[0].n >= sizeof(s->name)) {
        return fail(e, ln, FIELD[0], "empty or too long");
    }
    memcpy(s->name, field[0].p, field[0].n);
    s->name[field[0].n] = '\0';
    for (size_t i = 0; i < f->n; i++) {
        if (strcmp(f->sig[i].name, s->name) == 0) {
            return fail(e, ln, FIELD[0], "a signal named twice");
        }
    }
    if (span_is(field[1], "to_dut")) {
        s->dir = OTK_FIDELITY_TO_DUT;
    } else if (span_is(field[1], "from_dut")) {
        s->dir = OTK_FIDELITY_FROM_DUT;
    } else {
        return fail(e, ln, FIELD[1], "neither to_dut nor from_dut");
    }
    if (span_is(field[2], "rotary")) {
        s->transport = OTK_FIDELITY_ROTARY;
    } else if (span_is(field[2], "capture")) {
        s->transport = OTK_FIDELITY_CAPTURE;
    } else if (span_is(field[2], "analog")) {
        s->transport = OTK_FIDELITY_ANALOG;
    } else if (span_is(field[2], "level")) {
        s->transport = OTK_FIDELITY_LEVEL;
    } else {
        return fail(e, ln, FIELD[2], "not rotary, capture, analog or level");
    }
    for (unsigned c = 0; c < OTK_FIDELITY_NUM_COLUMNS; c++) {
        const Span v = field[3u + c];
        if (v.n == 0u) {
            continue; /* no requirement / not guaranteed */
        }
        const bool ok = c == OTK_FIDELITY_LOGIC_MV ? parse_mv(v, &s->value[c]) : parse_u64(v, &s->value[c]);
        if (!ok) {
            return fail(e, ln, FIELD[3u + c],
                        c == OTK_FIDELITY_LOGIC_MV ? "not volts with up to three decimals" : "not a whole number");
        }
        s->has[c] = true;
    }
    f->n++;
    return true;
}

bool otk_fidelity_parse(const char *text, size_t len, OtkFidelityFile *f, OtkFidelityError *err) {
    OtkFidelityError dummy;
    if (err == NULL) {
        err = &dummy;
    }
    memset(f, 0, sizeof(*f));
    memset(err, 0, sizeof(*err));
    if (text == NULL) {
        return fail(err, 1u, NULL, "no text");
    }
    unsigned ln = 0u;
    size_t pos = 0u;
    bool blank_seen = false;
    while (pos < len) {
        size_t end = pos;
        while (end < len && text[end] != '\n') {
            end++;
        }
        Span line = { text + pos, end - pos };
        if (line.n > 0u && line.p[line.n - 1u] == '\r') {
            line.n--;
        }
        pos = end + 1u;
        ln++;
        if (ln == 1u) {
            if (!parse_metadata(line, f, err)) {
                return false;
            }
        } else if (ln == 2u) {
            if (!span_is(line, HEADER)) {
                return fail(err, 2u, NULL, "not the header of version 1");
            }
        } else if (line.n == 0u) {
            blank_seen = true;
        } else if (blank_seen) {
            return fail(err, ln, NULL, "a row after a blank line");
        } else if (!parse_row(line, ln, f, err)) {
            return false;
        }
    }
    if (ln < 2u) {
        return fail(err, ln + 1u, NULL, "no header");
    }
    return true;
}

/* ---------------------------------------------------------------- the check */

static OtkFidelityVerdict verdict(OtkFidelityResult *r, OtkFidelityVerdict v, size_t mi, size_t bi,
                                  OtkFidelityColumn c, uint64_t req, uint64_t off) {
    r->verdict = v;
    r->machine_row = mi;
    r->bridge_row = bi;
    r->column = c;
    r->required = req;
    r->offered = off;
    return v;
}

OtkFidelityVerdict otk_fidelity_check(const OtkFidelityFile *machine, const OtkFidelityFile *bridge,
                                      OtkFidelityResult *r) {
    OtkFidelityResult dummy;
    if (r == NULL) {
        r = &dummy;
    }
    memset(r, 0, sizeof(*r));
    if (machine->side != OTK_FIDELITY_SIDE_MACHINE || bridge->side != OTK_FIDELITY_SIDE_BRIDGE) {
        return verdict(r, OTK_FIDELITY_WRONG_SIDE, 0u, 0u, OTK_FIDELITY_MAX_RATE_HZ, 0u, 0u);
    }
    bool taken[OTK_FIDELITY_MAX_SIGNALS] = { false };
    for (size_t mi = 0; mi < machine->n; mi++) {
        const OtkFidelitySignal *m = &machine->sig[mi];
        size_t bi = 0u;
        while (bi < bridge->n &&
               (taken[bi] || bridge->sig[bi].dir != m->dir || bridge->sig[bi].transport != m->transport)) {
            bi++;
        }
        if (bi == bridge->n) {
            return verdict(r, OTK_FIDELITY_NO_CHANNEL, mi, 0u, OTK_FIDELITY_MAX_RATE_HZ, 0u, 0u);
        }
        taken[bi] = true;
        const OtkFidelitySignal *b = &bridge->sig[bi];
        for (unsigned ci = 0; ci < OTK_FIDELITY_NUM_COLUMNS; ci++) {
            const OtkFidelityColumn c = (OtkFidelityColumn)ci;
            if (!m->has[c]) {
                continue;
            }
            const uint64_t req = m->value[c];
            bool have = b->has[c];
            uint64_t off = b->value[c];
            if (c == OTK_FIDELITY_LATENCY_NS && m->dir == OTK_FIDELITY_FROM_DUT) {
                /* what the DUT does reaches the plant after the run's delay, plus the channel's own */
                have = bridge->has_delay;
                off = bridge->dut_to_plant_delay_ns + (b->has[c] ? b->value[c] : 0u);
            }
            if (!have) {
                return verdict(r, OTK_FIDELITY_NOT_GUARANTEED, mi, bi, c, req, 0u);
            }
            bool met;
            switch (c) {
            case OTK_FIDELITY_MAX_RATE_HZ:
                met = req <= off;
                break;
            case OTK_FIDELITY_LOGIC_MV:
                met = req == off;
                break;
            case OTK_FIDELITY_RESOLUTION_NS:
            case OTK_FIDELITY_LATENCY_NS:
            case OTK_FIDELITY_JITTER_NS:
            case OTK_FIDELITY_NUM_COLUMNS:
            default:
                met = off <= req;
                break;
            }
            if (!met) {
                return verdict(r, OTK_FIDELITY_NOT_MET, mi, bi, c, req, off);
            }
        }
    }
    return verdict(r, OTK_FIDELITY_OK, 0u, 0u, OTK_FIDELITY_MAX_RATE_HZ, 0u, 0u);
}

/* ---------------------------------------------------------------- the message */

static void format_value(OtkFidelityColumn c, uint64_t v, char *out, size_t cap) {
    if (c == OTK_FIDELITY_LOGIC_MV) {
        (void)snprintf(out, cap, "%llu.%03llu V", (unsigned long long)(v / 1000u), (unsigned long long)(v % 1000u));
    } else {
        (void)snprintf(out, cap, "%llu %s", (unsigned long long)v, c == OTK_FIDELITY_MAX_RATE_HZ ? "Hz" : "ns");
    }
}

static const char *bound_word(OtkFidelityColumn c) {
    switch (c) {
    case OTK_FIDELITY_MAX_RATE_HZ:
        return "at least";
    case OTK_FIDELITY_LOGIC_MV:
        return "exactly";
    case OTK_FIDELITY_RESOLUTION_NS:
    case OTK_FIDELITY_LATENCY_NS:
    case OTK_FIDELITY_JITTER_NS:
    case OTK_FIDELITY_NUM_COLUMNS:
    default:
        return "at most";
    }
}

int otk_fidelity_describe(const OtkFidelityResult *r, const OtkFidelityFile *machine, const OtkFidelityFile *bridge,
                          char *buf, size_t cap) {
    static const char *const DIR[] = { "to_dut", "from_dut" };
    static const char *const TRANSPORT[] = { "rotary", "capture", "analog", "level" };
    if (r->verdict == OTK_FIDELITY_OK) {
        return snprintf(buf, cap, "fidelity: every signal is within what the bridge guarantees");
    }
    if (r->verdict == OTK_FIDELITY_WRONG_SIDE) {
        return snprintf(buf, cap, "fidelity: the machine's file must say side=machine and the bridge's side=bridge");
    }
    const OtkFidelitySignal *m = &machine->sig[r->machine_row];
    if (r->verdict == OTK_FIDELITY_NO_CHANNEL) {
        return snprintf(buf, cap, "fidelity: signal %s (line %u): the bridge has no %s %s channel left for it",
                        m->name, m->line, DIR[m->dir], TRANSPORT[m->transport]);
    }
    const OtkFidelitySignal *b = &bridge->sig[r->bridge_row];
    char req[40];
    format_value(r->column, r->required, req, sizeof(req));
    if (r->verdict == OTK_FIDELITY_NOT_GUARANTEED) {
        return snprintf(buf, cap, "fidelity: signal %s (line %u), column %s: requires %s %s; the bridge's %s does not "
                                  "guarantee it",
                        m->name, m->line, otk_fidelity_column_name(r->column), bound_word(r->column), req, b->name);
    }
    char off[40];
    format_value(r->column, r->offered, off, sizeof(off));
    const bool via_delay = r->column == OTK_FIDELITY_LATENCY_NS && m->dir == OTK_FIDELITY_FROM_DUT;
    return snprintf(buf, cap, "fidelity: signal %s (line %u), column %s: requires %s %s; the bridge's %s gives %s%s",
                    m->name, m->line, otk_fidelity_column_name(r->column), bound_word(r->column), req, b->name, off,
                    via_delay ? " (its DUT-to-plant delay included)" : "");
}
