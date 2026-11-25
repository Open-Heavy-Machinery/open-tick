/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_link.c
 * @brief Links: each transport fault, its filter, delivery order, overflow,
 *        the PRNG's determinism and the scenario signals.
 */

#include <string.h>

#include "check.h"
#include <opentick/link.h>

#define MS(x) otk_ns_from_ms(x)

static OtkCanFrame frame(uint32_t id, uint8_t flags, uint8_t len, uint8_t tag) {
    OtkCanFrame f;
    memset(&f, 0, sizeof(f));
    f.id = id;
    f.flags = flags;
    f.len = len;
    for (uint8_t i = 0U; i < len; i++) {
        f.data[i] = (uint8_t)(tag + i);
    }
    return f;
}

static OtkLink g_link; /* large: keep it off the stack */

static void reset(size_t cap) {
    otk_link_init(&g_link, cap, 42U);
}

static void set(const OtkLinkFaults *faults) {
    otk_link_set_faults(&g_link, faults);
}

/* Pops everything due at @p now into @p out; returns how many. */
static size_t pop_all(OtkNs now, OtkCanFrame *out, size_t cap) {
    size_t n = 0U;
    OtkCanFrame f;
    while (n < cap && otk_link_pop(&g_link, now, &f)) {
        out[n++] = f;
    }
    return n;
}

static const OtkLinkFilter ONLY_0X100 = { true, false, 0x100U, 0x7FFU };

static void test_transparent(void) {
    reset(OTK_CAN_QUEUE_MAX);
    for (uint8_t i = 0U; i < 5U; i++) {
        const OtkCanFrame f = frame(0x100U + (uint32_t)(i % 2U), 0U, 8U, (uint8_t)(10U * i));
        CHECK(otk_link_push(&g_link, &f, MS(10)));
    }
    OtkCanFrame out[8];
    CHECK(pop_all(MS(9), out, 8U) == 0U); /* nothing before it was sent */
    CHECK(pop_all(MS(10), out, 8U) == 5U);
    for (uint8_t i = 0U; i < 5U; i++) {
        CHECK(out[i].id == 0x100U + (uint32_t)(i % 2U));
        CHECK(out[i].data[0] == (uint8_t)(10U * i)); /* unchanged, in push order */
        CHECK(out[i].data[7] == (uint8_t)(10U * i + 7U));
    }
    CHECK(otk_link_count(&g_link) == 0U);
    CHECK(g_link.dropped == 0U && g_link.corrupted == 0U && g_link.overflows == 0U && g_link.bus_off_lost == 0U);
}

static void test_drop_and_filter(void) {
    reset(OTK_CAN_QUEUE_MAX);
    otk_link_set_filter(&g_link, OTK_LINK_DROP, &ONLY_0X100);
    set(&(OtkLinkFaults){ .drop = true });
    const OtkCanFrame a = frame(0x100U, 0U, 2U, 1U);
    const OtkCanFrame b = frame(0x101U, 0U, 2U, 2U);
    const OtkCanFrame c = frame(0x100U, OTK_CAN_EXTENDED, 2U, 3U); /* same number, other format */
    CHECK(!otk_link_push(&g_link, &a, 0U));
    CHECK(otk_link_push(&g_link, &b, 0U));
    CHECK(otk_link_push(&g_link, &c, 0U));
    CHECK(g_link.dropped == 1U);
    OtkCanFrame out[4];
    CHECK(pop_all(0U, out, 4U) == 2U);
    CHECK(out[0].id == 0x101U && out[1].id == 0x100U && (out[1].flags & OTK_CAN_EXTENDED) != 0U);

    /* The mask: 0x100/0x7F0 matches 0x100..0x10F. */
    otk_link_set_filter(&g_link, OTK_LINK_DROP, &(OtkLinkFilter){ true, false, 0x100U, 0x7F0U });
    const OtkCanFrame d = frame(0x10FU, 0U, 2U, 4U);
    const OtkCanFrame e = frame(0x110U, 0U, 2U, 5U);
    CHECK(!otk_link_push(&g_link, &d, 0U));
    CHECK(otk_link_push(&g_link, &e, 0U));

    /* No filter: every frame. Off: none. */
    otk_link_set_filter(&g_link, OTK_LINK_DROP, NULL);
    CHECK(!otk_link_push(&g_link, &e, 0U));
    set(&(OtkLinkFaults){ 0 });
    CHECK(otk_link_push(&g_link, &e, 0U));
}

static void test_drop_one_in(void) {
    reset(OTK_CAN_QUEUE_MAX);
    otk_link_set_filter(&g_link, OTK_LINK_DROP_ONE_IN, &ONLY_0X100);
    set(&(OtkLinkFaults){ .drop_one_in = 3U });
    const OtkCanFrame a = frame(0x100U, 0U, 2U, 0U);
    const OtkCanFrame other = frame(0x200U, 0U, 2U, 0U);
    bool passed[9];
    for (size_t i = 0; i < 9U; i++) {
        CHECK(otk_link_push(&g_link, &other, 0U)); /* not matching: never lost, not counted */
        passed[i] = otk_link_push(&g_link, &a, 0U);
        OtkCanFrame out[4];
        (void)pop_all(0U, out, 4U);
    }
    for (size_t i = 0; i < 9U; i++) {
        CHECK(passed[i] == ((i % 3U) != 2U)); /* the 3rd, 6th, 9th are lost */
    }
    CHECK(g_link.dropped == 3U);

    /* Changing N restarts the count. */
    CHECK(otk_link_push(&g_link, &a, 0U));
    set(&(OtkLinkFaults){ .drop_one_in = 2U });
    CHECK(otk_link_push(&g_link, &a, 0U));
    CHECK(!otk_link_push(&g_link, &a, 0U));
    set(&(OtkLinkFaults){ .drop_one_in = 1U });
    CHECK(!otk_link_push(&g_link, &a, 0U));
}

static void test_delay(void) {
    reset(OTK_CAN_QUEUE_MAX);
    otk_link_set_filter(&g_link, OTK_LINK_DELAY, &ONLY_0X100);
    set(&(OtkLinkFaults){ .delay = MS(30) });
    const OtkCanFrame a = frame(0x100U, 0U, 2U, 1U);
    const OtkCanFrame b = frame(0x101U, 0U, 2U, 2U);
    CHECK(otk_link_push(&g_link, &a, MS(10)));
    CHECK(otk_link_push(&g_link, &b, MS(10)));
    OtkCanFrame out[4];
    CHECK(pop_all(MS(10), out, 4U) == 1U && out[0].id == 0x101U); /* b overtakes a: other id */
    CHECK(pop_all(MS(39), out, 4U) == 0U);
    CHECK(pop_all(MS(40), out, 4U) == 1U && out[0].id == 0x100U);
    CHECK(out[0].data[0] == 1U);
}

static void test_same_id_never_overtakes(void) {
    reset(OTK_CAN_QUEUE_MAX);
    set(&(OtkLinkFaults){ .delay = MS(100) });
    const OtkCanFrame a = frame(0x100U, 0U, 2U, 1U);
    const OtkCanFrame b = frame(0x100U, 0U, 2U, 2U);
    const OtkCanFrame c = frame(0x101U, 0U, 2U, 3U);
    CHECK(otk_link_push(&g_link, &a, MS(0))); /* due at 100 ms */
    set(&(OtkLinkFaults){ 0 });               /* the delay drops while a is in flight */
    CHECK(otk_link_push(&g_link, &b, MS(10))); /* would be due at 10 ms: held behind a */
    CHECK(otk_link_push(&g_link, &c, MS(10))); /* other id: due at 10 ms */
    OtkCanFrame out[4];
    CHECK(pop_all(MS(10), out, 4U) == 1U && out[0].id == 0x101U);
    CHECK(pop_all(MS(99), out, 4U) == 0U);
    CHECK(pop_all(MS(100), out, 4U) == 2U);
    CHECK(out[0].data[0] == 1U && out[1].data[0] == 2U); /* a, then b */
}

static void test_jitter(void) {
    reset(OTK_CAN_QUEUE_MAX);
    set(&(OtkLinkFaults){ .jitter = MS(50) });
    OtkNs due[32];
    bool spread = false;
    for (size_t i = 0; i < 32U; i++) {
        const OtkCanFrame f = frame(0x300U + (uint32_t)i, 0U, 1U, 0U); /* all different ids */
        CHECK(otk_link_push(&g_link, &f, MS(10)));
        due[i] = g_link.line[i].due;
        CHECK(due[i] >= MS(10) && due[i] <= MS(60));
        spread = spread || (i > 0U && due[i] != due[0]);
    }
    CHECK(spread);

    /* Same seed, same frames: the same delays. Another seed: others. */
    otk_link_init(&g_link, OTK_CAN_QUEUE_MAX, 42U);
    set(&(OtkLinkFaults){ .jitter = MS(50) });
    bool same = true;
    for (size_t i = 0; i < 32U; i++) {
        const OtkCanFrame f = frame(0x300U + (uint32_t)i, 0U, 1U, 0U);
        CHECK(otk_link_push(&g_link, &f, MS(10)));
        same = same && g_link.line[i].due == due[i];
    }
    CHECK(same);
    otk_link_init(&g_link, OTK_CAN_QUEUE_MAX, 43U);
    set(&(OtkLinkFaults){ .jitter = MS(50) });
    bool differs = false;
    for (size_t i = 0; i < 32U; i++) {
        const OtkCanFrame f = frame(0x300U + (uint32_t)i, 0U, 1U, 0U);
        CHECK(otk_link_push(&g_link, &f, MS(10)));
        differs = differs || g_link.line[i].due != due[i];
    }
    CHECK(differs);

    /* With one id, jitter never reorders: frames come out in push order. */
    reset(OTK_CAN_QUEUE_MAX);
    set(&(OtkLinkFaults){ .jitter = MS(50) });
    for (uint8_t i = 0U; i < 32U; i++) {
        const OtkCanFrame f = frame(0x100U, 0U, 1U, i);
        CHECK(otk_link_push(&g_link, &f, MS(i)));
    }
    OtkCanFrame out[32];
    CHECK(pop_all(MS(100), out, 32U) == 32U);
    for (uint8_t i = 0U; i < 32U; i++) {
        CHECK(out[i].data[0] == i);
    }

    /* The filter: a frame that does not match is not delayed. */
    reset(OTK_CAN_QUEUE_MAX);
    otk_link_set_filter(&g_link, OTK_LINK_JITTER, &ONLY_0X100);
    set(&(OtkLinkFaults){ .jitter = MS(50) });
    const OtkCanFrame other = frame(0x200U, 0U, 1U, 0U);
    for (size_t i = 0; i < 16U; i++) {
        CHECK(otk_link_push(&g_link, &other, MS(10)));
        CHECK(g_link.line[i].due == MS(10));
    }
}

static void test_corrupt(void) {
    reset(OTK_CAN_QUEUE_MAX);
    otk_link_set_filter(&g_link, OTK_LINK_CORRUPT, &ONLY_0X100);
    set(&(OtkLinkFaults){ .corrupt = 1U });
    const OtkCanFrame a = frame(0x100U, 0U, 8U, 0x10U);
    const OtkCanFrame other = frame(0x101U, 0U, 8U, 0x10U);
    CHECK(otk_link_push(&g_link, &a, 0U));
    CHECK(otk_link_push(&g_link, &other, 0U));
    OtkCanFrame out[4];
    CHECK(pop_all(0U, out, 4U) == 2U);
    CHECK(out[0].data[0] == (uint8_t)(0x10U ^ 0xFFU)); /* byte 1 flipped */
    for (uint8_t i = 1U; i < 8U; i++) {
        CHECK(out[0].data[i] == (uint8_t)(0x10U + i)); /* the others untouched */
    }
    CHECK(memcmp(out[1].data, other.data, 8U) == 0); /* not matching */
    CHECK(g_link.corrupted == 1U);

    set(&(OtkLinkFaults){ .corrupt = 8U });
    CHECK(otk_link_push(&g_link, &a, 0U));
    CHECK(pop_all(0U, out, 4U) == 1U);
    CHECK(out[0].data[7] == (uint8_t)(0x17U ^ 0xFFU) && out[0].data[0] == 0x10U);

    /* A frame shorter than N passes unchanged. */
    set(&(OtkLinkFaults){ .corrupt = 3U });
    const OtkCanFrame shorter = frame(0x100U, 0U, 2U, 0x10U);
    CHECK(otk_link_push(&g_link, &shorter, 0U));
    CHECK(pop_all(0U, out, 4U) == 1U);
    CHECK(memcmp(out[0].data, shorter.data, OTK_CAN_MAX_LEN) == 0);
    CHECK(g_link.corrupted == 2U);
}

static void test_bus_off(void) {
    reset(OTK_CAN_QUEUE_MAX);
    otk_link_set_filter(&g_link, OTK_LINK_BUS_OFF, &ONLY_0X100); /* not used: the whole direction */
    const OtkCanFrame a = frame(0x100U, 0U, 2U, 1U);
    const OtkCanFrame b = frame(0x200U, 0U, 2U, 2U);
    set(&(OtkLinkFaults){ .delay = MS(20) });
    CHECK(otk_link_push(&g_link, &a, MS(0))); /* due at 20 ms, while bus-off */
    CHECK(otk_link_push(&g_link, &b, MS(20))); /* due at 40 ms, after it */
    set(&(OtkLinkFaults){ .delay = MS(20), .bus_off = true });
    CHECK(otk_link_bus_off(&g_link));
    CHECK(!otk_link_push(&g_link, &a, MS(10)));
    CHECK(!otk_link_push(&g_link, &b, MS(10)));
    OtkCanFrame out[4];
    CHECK(pop_all(MS(30), out, 4U) == 0U);
    CHECK(g_link.bus_off_lost == 3U); /* two pushed, one fallen due */
    set(&(OtkLinkFaults){ .delay = MS(20) });
    CHECK(!otk_link_bus_off(&g_link));
    CHECK(pop_all(MS(40), out, 4U) == 1U && out[0].id == 0x200U);
    CHECK(otk_link_push(&g_link, &a, MS(40)));
}

static void test_overflow(void) {
    reset(4U);
    set(&(OtkLinkFaults){ .delay = MS(100) });
    const OtkCanFrame a = frame(0x100U, 0U, 2U, 1U);
    for (size_t i = 0; i < 4U; i++) {
        CHECK(otk_link_push(&g_link, &a, 0U));
    }
    CHECK(!otk_link_push(&g_link, &a, 0U));
    CHECK(g_link.overflows == 1U && otk_link_count(&g_link) == 4U);
    OtkCanFrame out[8];
    CHECK(pop_all(MS(100), out, 8U) == 4U);
    CHECK(otk_link_push(&g_link, &a, 0U));

    otk_link_init(&g_link, 0U, 1U);
    CHECK(g_link.cap == 1U);
    otk_link_init(&g_link, 1000U, 1U);
    CHECK(g_link.cap == OTK_CAN_QUEUE_MAX);
}

enum { SIG_OTHER, SIG_LINK, SIG_COUNT = SIG_LINK + OTK_LINK_SIGNAL_COUNT };
static const char *const NAMES[SIG_COUNT] = {
    [SIG_OTHER] = "target_rpm",
    [SIG_LINK] = OTK_LINK_SIGNAL_NAMES("speed"),
};

static void test_signals(void) {
    const OtkSignalTable table = { NAMES, SIG_COUNT };
    char err[128];
    CHECK(otk_signal_table_check(&table, err, sizeof(err)));
    CHECK(strcmp(NAMES[SIG_LINK + OTK_LINK_DROP], "fault.link.speed.drop") == 0);
    CHECK(strcmp(NAMES[SIG_LINK + OTK_LINK_DROP_ONE_IN], "fault.link.speed.drop_one_in") == 0);
    CHECK(strcmp(NAMES[SIG_LINK + OTK_LINK_DELAY], "fault.link.speed.delay_ms") == 0);
    CHECK(strcmp(NAMES[SIG_LINK + OTK_LINK_JITTER], "fault.link.speed.jitter_ms") == 0);
    CHECK(strcmp(NAMES[SIG_LINK + OTK_LINK_CORRUPT], "fault.link.speed.corrupt") == 0);
    CHECK(strcmp(NAMES[SIG_LINK + OTK_LINK_BUS_OFF], "fault.link.speed.bus_off") == 0);

    OtkLinkFaults f;
    memset(&f, 0, sizeof(f));
    CHECK(!otk_link_signal(&f, SIG_LINK, SIG_OTHER, 1.0));
    CHECK(!otk_link_signal(&f, SIG_LINK, SIG_COUNT, 1.0));
    CHECK(otk_link_signal(&f, SIG_LINK, SIG_LINK + OTK_LINK_DROP, 1.0) && f.drop);
    CHECK(otk_link_signal(&f, SIG_LINK, SIG_LINK + OTK_LINK_DROP_ONE_IN, 4.0) && f.drop_one_in == 4U);
    CHECK(otk_link_signal(&f, SIG_LINK, SIG_LINK + OTK_LINK_DELAY, 300.0) && f.delay == MS(300));
    CHECK(otk_link_signal(&f, SIG_LINK, SIG_LINK + OTK_LINK_JITTER, 0.5) && f.jitter == otk_ns_from_us(500));
    CHECK(otk_link_signal(&f, SIG_LINK, SIG_LINK + OTK_LINK_CORRUPT, 3.0) && f.corrupt == 3U);
    CHECK(otk_link_signal(&f, SIG_LINK, SIG_LINK + OTK_LINK_BUS_OFF, 1.0) && f.bus_off);

    /* Off again; negative values clamp to off. */
    CHECK(otk_link_signal(&f, SIG_LINK, SIG_LINK + OTK_LINK_DROP, 0.0) && !f.drop);
    CHECK(otk_link_signal(&f, SIG_LINK, SIG_LINK + OTK_LINK_DROP_ONE_IN, -2.0) && f.drop_one_in == 0U);
    CHECK(otk_link_signal(&f, SIG_LINK, SIG_LINK + OTK_LINK_DELAY, -5.0) && f.delay == 0U);
    CHECK(otk_link_signal(&f, SIG_LINK, SIG_LINK + OTK_LINK_CORRUPT, 0.0) && f.corrupt == 0U);
    CHECK(otk_link_signal(&f, SIG_LINK, SIG_LINK + OTK_LINK_CORRUPT, 1000.0) && f.corrupt == 255U);
    CHECK(otk_link_signal(&f, SIG_LINK, SIG_LINK + OTK_LINK_BUS_OFF, 0.0) && !f.bus_off);
}

int main(void) {
    RUN_TEST(test_transparent);
    RUN_TEST(test_drop_and_filter);
    RUN_TEST(test_drop_one_in);
    RUN_TEST(test_delay);
    RUN_TEST(test_same_id_never_overtakes);
    RUN_TEST(test_jitter);
    RUN_TEST(test_corrupt);
    RUN_TEST(test_bus_off);
    RUN_TEST(test_overflow);
    RUN_TEST(test_signals);
    return check_report();
}
