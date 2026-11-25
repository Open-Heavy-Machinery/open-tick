/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file link.c
 * @brief One direction of a CAN path, with transport faults (opentick/link.h).
 */

#include <opentick/link.h>

#include <string.h>

void otk_link_init(OtkLink *link, size_t cap, uint64_t seed) {
    memset(link, 0, sizeof(*link));
    link->cap = (cap < 1U) ? 1U : (cap > OTK_CAN_QUEUE_MAX) ? OTK_CAN_QUEUE_MAX : cap;
    link->rng = seed;
}

void otk_link_set_filter(OtkLink *link, OtkLinkFault fault, const OtkLinkFilter *filter) {
    if ((unsigned)fault >= (unsigned)OTK_LINK_FAULT_COUNT) {
        return;
    }
    if (filter == NULL) {
        memset(&link->filters[fault], 0, sizeof(link->filters[fault]));
    } else {
        link->filters[fault] = *filter;
    }
}

void otk_link_set_faults(OtkLink *link, const OtkLinkFaults *faults) {
    if (faults->drop_one_in != link->faults.drop_one_in) {
        link->matched_one_in = 0U;
    }
    link->faults = *faults;
}

static bool matches(const OtkLink *link, OtkLinkFault fault, const OtkCanFrame *f) {
    const OtkLinkFilter *flt = &link->filters[fault];
    if (!flt->on) {
        return true;
    }
    const bool extended = (f->flags & OTK_CAN_EXTENDED) != 0U;
    return extended == flt->extended && ((f->id ^ flt->id) & flt->mask) == 0U;
}

static bool same_id(const OtkCanFrame *a, const OtkCanFrame *b) {
    return a->id == b->id && ((a->flags ^ b->flags) & OTK_CAN_EXTENDED) == 0U;
}

/* splitmix64: small, fast and the same on every host. */
static uint64_t next_random(OtkLink *link) {
    link->rng += UINT64_C(0x9E3779B97F4A7C15);
    uint64_t z = link->rng;
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

static OtkNs add_sat(OtkNs a, OtkNs b) {
    return (a > UINT64_MAX - b) ? UINT64_MAX : a + b;
}

bool otk_link_push(OtkLink *link, const OtkCanFrame *frame, OtkNs t) {
    const OtkLinkFaults *fl = &link->faults;
    if (fl->bus_off) {
        link->bus_off_lost++;
        return false;
    }
    if (fl->drop && matches(link, OTK_LINK_DROP, frame)) {
        link->dropped++;
        return false;
    }
    if (fl->drop_one_in > 0U && matches(link, OTK_LINK_DROP_ONE_IN, frame)) {
        link->matched_one_in++;
        if (link->matched_one_in >= fl->drop_one_in) {
            link->matched_one_in = 0U;
            link->dropped++;
            return false;
        }
    }
    if (link->n >= link->cap) {
        link->overflows++;
        return false;
    }

    OtkLinkSlot *s = &link->line[link->n];
    s->frame = *frame;
    if (fl->corrupt > 0U && fl->corrupt <= frame->len && matches(link, OTK_LINK_CORRUPT, frame)) {
        s->frame.data[fl->corrupt - 1U] ^= 0xFFU;
        link->corrupted++;
    }
    OtkNs due = t;
    if (fl->delay > 0U && matches(link, OTK_LINK_DELAY, frame)) {
        due = add_sat(due, fl->delay);
    }
    if (fl->jitter > 0U && matches(link, OTK_LINK_JITTER, frame)) {
        const uint64_t r = next_random(link);
        due = add_sat(due, (fl->jitter == UINT64_MAX) ? r : r % (fl->jitter + 1U));
    }
    /* Never before an earlier frame with the same identifier still in flight. */
    for (size_t i = 0; i < link->n; i++) {
        if (same_id(&link->line[i].frame, frame) && link->line[i].due > due) {
            due = link->line[i].due;
        }
    }
    s->due = due;
    link->n++;
    return true;
}

bool otk_link_pop(OtkLink *link, OtkNs now, OtkCanFrame *out) {
    for (;;) {
        size_t best = link->n;
        for (size_t i = 0; i < link->n; i++) {
            if (link->line[i].due <= now && (best == link->n || link->line[i].due < link->line[best].due)) {
                best = i; /* strict <: on a tie the earlier push wins */
            }
        }
        if (best == link->n) {
            return false;
        }
        const OtkCanFrame f = link->line[best].frame;
        memmove(&link->line[best], &link->line[best + 1U], (link->n - best - 1U) * sizeof(link->line[0]));
        link->n--;
        if (link->faults.bus_off) {
            link->bus_off_lost++;
            continue; /* due while the bus is off: lost */
        }
        *out = f;
        return true;
    }
}

size_t otk_link_count(const OtkLink *link) {
    return link->n;
}

bool otk_link_bus_off(const OtkLink *link) {
    return link->faults.bus_off;
}

static uint32_t to_u32(double v) {
    if (!(v > 0.0)) {
        return 0U; /* also NaN */
    }
    return (v >= 4294967295.0) ? UINT32_MAX : (uint32_t)(v + 0.5);
}

static OtkNs ms_to_ns(double ms) {
    if (!(ms > 0.0)) {
        return 0U;
    }
    const double ns = ms * 1e6;
    return (ns >= 1.8e19) ? UINT64_MAX : (OtkNs)(ns + 0.5);
}

bool otk_link_signal(OtkLinkFaults *faults, OtkSignalId base, OtkSignalId signal, double value) {
    if (signal < base || signal - base >= OTK_LINK_SIGNAL_COUNT) {
        return false;
    }
    switch ((OtkLinkFault)(signal - base)) {
    case OTK_LINK_DROP:
        faults->drop = value != 0.0;
        break;
    case OTK_LINK_DROP_ONE_IN:
        faults->drop_one_in = to_u32(value);
        break;
    case OTK_LINK_DELAY:
        faults->delay = ms_to_ns(value);
        break;
    case OTK_LINK_JITTER:
        faults->jitter = ms_to_ns(value);
        break;
    case OTK_LINK_CORRUPT: {
        const uint32_t n = to_u32(value);
        faults->corrupt = (uint8_t)((n > 255U) ? 255U : n); /* past any frame: never flips */
        break;
    }
    case OTK_LINK_BUS_OFF:
        faults->bus_off = value != 0.0;
        break;
    default:
        break;
    }
    return true;
}
