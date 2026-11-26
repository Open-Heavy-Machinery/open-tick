/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_bridge.c
 * @brief The host <-> bridge protocol v1 codec (opentick/bridge.h) against
 *        docs/bridge-protocol.md: CRC, byte layout, every message, the
 *        budget, the parser's resynchronisation and the unit conversions.
 */

#include <string.h>

#include "check.h"
#include <opentick/bridge.h>

/* Frames a payload and parses it back in one call. */
static bool roundtrip(uint8_t type, uint16_t seq, const uint8_t *payload, size_t len, OtkBridgeParser *p,
                      OtkBridgeMessage *msg) {
    uint8_t frame[OTK_BRIDGE_FRAMING_BYTES + OTK_BRIDGE_MAX_PAYLOAD];
    const size_t n = otk_bridge_frame(type, seq, payload, len, frame, sizeof(frame));
    size_t used = 0;
    otk_bridge_parser_init(p);
    return n > 0 && otk_bridge_parse(p, frame, n, &used, msg) && used == n;
}

static void test_crc(void) {
    const uint8_t check[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    CHECK(otk_bridge_crc16(check, sizeof(check)) == 0x29B1u); /* CRC-16/IBM-3740 check value */
    CHECK(otk_bridge_crc16(check, 0) == 0xFFFFu);
    const uint8_t a = 'A';
    CHECK(otk_bridge_crc16(&a, 1) == 0xB915u);
}

static void test_layout(void) {
    /* A request with one setpoint, byte for byte (§3.1, §3.2). */
    OtkBridgeRequest r;
    memset(&r, 0, sizeof(r));
    r.first_step = 0x01020304u;
    r.n = 1;
    r.sp[0].crank_phase_inc = 0x11223344u;
    r.sp[0].analog[0] = 0x0001u;
    r.sp[0].analog[1] = 0x0203u;
    r.sp[0].analog[2] = 0x8000u;
    r.sp[0].analog[3] = 0xFFFFu;
    uint8_t payload[64];
    const size_t len = otk_bridge_encode_request(&r, payload, sizeof(payload));
    CHECK(len == 17u);
    const uint8_t want_payload[17] = { 0x04, 0x03, 0x02, 0x01, 0x01, 0x44, 0x33, 0x22, 0x11,
                                       0x01, 0x00, 0x03, 0x02, 0x00, 0x80, 0xFF, 0xFF };
    CHECK(memcmp(payload, want_payload, sizeof(want_payload)) == 0);

    uint8_t frame[64];
    const size_t n = otk_bridge_frame(OTK_BRIDGE_EXCHANGE, 0x1234u, payload, len, frame, sizeof(frame));
    CHECK(n == 26u);
    const uint8_t want_header[7] = { 0xA5, 0x01, 0x01, 0x34, 0x12, 0x11, 0x00 };
    CHECK(memcmp(frame, want_header, sizeof(want_header)) == 0);
    CHECK(memcmp(&frame[7], want_payload, sizeof(want_payload)) == 0);
    const uint16_t crc = otk_bridge_crc16(&frame[1], 6u + len); /* version .. end of payload */
    CHECK(frame[24] == (uint8_t)(crc & 0xFFu) && frame[25] == (uint8_t)(crc >> 8));

    CHECK(otk_bridge_frame(OTK_BRIDGE_EXCHANGE, 0, payload, len, frame, 25u) == 0); /* no room */
    CHECK(otk_bridge_frame(OTK_BRIDGE_START, 7u, NULL, 0, frame, sizeof(frame)) == 9u);
}

static void test_request_and_budget(void) {
    OtkBridgeRequest r, d;
    memset(&r, 0, sizeof(r));
    r.first_step = 42u;
    r.n = OTK_BRIDGE_MAX_SETPOINTS;
    for (unsigned i = 0; i < r.n; i++) {
        r.sp[i].crank_phase_inc = 208499983u + i;
        for (unsigned k = 0; k < OTK_BRIDGE_ANALOG; k++) {
            r.sp[i].analog[k] = (uint16_t)(1000u * i + k);
        }
    }
    uint8_t payload[OTK_BRIDGE_MAX_PAYLOAD];
    const size_t len = otk_bridge_encode_request(&r, payload, sizeof(payload));
    CHECK(len + OTK_BRIDGE_FRAMING_BYTES == 446u); /* §6: the largest request */
    CHECK(len + OTK_BRIDGE_FRAMING_BYTES <= OTK_BRIDGE_BUDGET_BL616);
    CHECK(otk_bridge_decode_request(payload, len, &d));
    CHECK(d.first_step == 42u && d.n == r.n);
    CHECK(memcmp(d.sp, r.sp, sizeof(r.sp[0]) * r.n) == 0);

    r.n = 16; /* §6: H = 16 */
    CHECK(otk_bridge_encode_request(&r, payload, sizeof(payload)) + OTK_BRIDGE_FRAMING_BYTES == 206u);

    r.n = OTK_BRIDGE_MAX_SETPOINTS + 1u;
    CHECK(otk_bridge_encode_request(&r, payload, sizeof(payload)) == 0);
    r.n = 2;
    CHECK(otk_bridge_encode_request(&r, payload, 28u) == 0); /* 29 needed */

    /* The decoder checks the length exactly. */
    r.n = 2;
    const size_t l2 = otk_bridge_encode_request(&r, payload, sizeof(payload));
    CHECK(!otk_bridge_decode_request(payload, l2 - 1u, &d));
    CHECK(!otk_bridge_decode_request(payload, l2 + 1u, &d));
    payload[4] = OTK_BRIDGE_MAX_SETPOINTS + 1u;
    CHECK(!otk_bridge_decode_request(payload, 5u + (size_t)payload[4] * OTK_BRIDGE_SETPOINT_BYTES, &d));
}

static void test_reply(void) {
    OtkBridgeReply r, d;
    memset(&r, 0, sizeof(r));
    r.t2 = 0x0102030405060708ull;
    r.t3 = 0x1112131415161718ull;
    r.step = 59992u;
    r.state = OTK_BRIDGE_RUNNING;
    r.fill = 16u;
    r.underruns = 0xFFFFFFFFu;
    r.error = OTK_BRIDGE_ERR_LATE | OTK_BRIDGE_ERR_CAPTURE_OVERFLOW;
    r.error_count = 3u;
    r.applied.crank_phase_inc = 123456789u;
    r.applied.analog[2] = 777u;
    r.n_cap = 8;
    for (unsigned i = 0; i < r.n_cap; i++) {
        r.cap[i] = (OtkBridgeCapture){ (uint8_t)i, 0xF0000000u + i, 27000u + i };
    }
    uint8_t payload[OTK_BRIDGE_MAX_PAYLOAD];
    const size_t len = otk_bridge_encode_reply(&r, payload, sizeof(payload));
    CHECK(len + OTK_BRIDGE_FRAMING_BYTES == 125u); /* §6: a reply with 8 captures */
    CHECK(payload[20] == OTK_BRIDGE_RUNNING);      /* §3.3: state after step */
    CHECK(otk_bridge_decode_reply(payload, len, &d));
    CHECK(d.t2 == r.t2 && d.t3 == r.t3 && d.step == r.step && d.state == r.state && d.fill == r.fill);
    CHECK(d.underruns == r.underruns && d.error == r.error && d.error_count == r.error_count);
    CHECK(d.applied.crank_phase_inc == r.applied.crank_phase_inc && d.applied.analog[2] == 777u);
    CHECK(d.n_cap == 8u);
    for (unsigned i = 0; i < d.n_cap; i++) {
        CHECK(d.cap[i].channel == i && d.cap[i].start == 0xF0000000u + i && d.cap[i].width == 27000u + i);
    }
    CHECK(!otk_bridge_decode_reply(payload, len - 1u, &d));
    r.n_cap = OTK_BRIDGE_MAX_CAPTURES + 1u;
    CHECK(otk_bridge_encode_reply(&r, payload, sizeof(payload)) == 0);
    r.n_cap = 0;
    CHECK(otk_bridge_encode_reply(&r, payload, sizeof(payload)) == OTK_BRIDGE_REPLY_FIXED_BYTES);
}

static void test_control_messages(void) {
    uint8_t p[64];
    const OtkBridgeConfig c = { 27000u, 60u, 2u, 0x10000000u, 0x00000000u, 0x80000000u, 0x3Fu, 0x3Fu, 100u };
    OtkBridgeConfig dc;
    CHECK(otk_bridge_encode_configure(&c, p, sizeof(p)) == OTK_BRIDGE_CONFIGURE_BYTES);
    CHECK(p[0] == 0x78 && p[1] == 0x69 && p[4] == 60u && p[5] == 2u && p[20] == 100u); /* 27000 = 0x6978 */
    CHECK(otk_bridge_decode_configure(p, OTK_BRIDGE_CONFIGURE_BYTES, &dc));
    CHECK(dc.step_ticks == 27000u && dc.crank_teeth == 60u && dc.crank_missing == 2u && dc.crank_offset == c.crank_offset);
    CHECK(dc.cam_start == 0u && dc.cam_end == 0x80000000u && dc.capture_mask == 0x3Fu);
    CHECK(dc.capture_polarity == 0x3Fu && dc.link_timeout_steps == 100u);
    CHECK(!otk_bridge_decode_configure(p, OTK_BRIDGE_CONFIGURE_BYTES - 1u, &dc));
    CHECK(otk_bridge_encode_configure(&c, p, OTK_BRIDGE_CONFIGURE_BYTES - 1u) == 0);

    const OtkBridgeConfigReply cr = { OTK_BRIDGE_STATUS_OK, 27000000u, 6u, 4u, 12u };
    OtkBridgeConfigReply dcr;
    CHECK(otk_bridge_encode_configure_reply(&cr, p, sizeof(p)) == 8u);
    CHECK(otk_bridge_decode_configure_reply(p, 8u, &dcr));
    CHECK(dcr.tick_hz == 27000000u && dcr.n_capture == 6u && dcr.n_analog == 4u && dcr.analog_bits == 12u);

    const OtkBridgeStartReply sr = { OTK_BRIDGE_STATUS_OK, 0x0000000123456789ull };
    OtkBridgeStartReply dsr;
    CHECK(otk_bridge_encode_start_reply(&sr, p, sizeof(p)) == 9u);
    CHECK(otk_bridge_decode_start_reply(p, 9u, &dsr) && dsr.t_start == sr.t_start && dsr.status == 0u);

    const OtkBridgeStopReply st = { OTK_BRIDGE_STATUS_WRONG_STATE, 60000u, 7u };
    OtkBridgeStopReply dst;
    CHECK(otk_bridge_encode_stop_reply(&st, p, sizeof(p)) == 9u);
    CHECK(otk_bridge_decode_stop_reply(p, 9u, &dst) && dst.status == 1u && dst.step == 60000u && dst.underruns == 7u);
    CHECK(!otk_bridge_decode_stop_reply(p, 8u, &dst));
}

static void test_every_type_through_the_parser(void) {
    OtkBridgeParser p;
    OtkBridgeMessage m;
    uint8_t payload[OTK_BRIDGE_MAX_PAYLOAD];
    const OtkBridgeConfig c = { 27000u, 60u, 2u, 0u, 0u, 0u, 1u, 1u, 100u };
    const size_t len = otk_bridge_encode_configure(&c, payload, sizeof(payload));
    CHECK(roundtrip(OTK_BRIDGE_CONFIGURE, 9u, payload, len, &p, &m));
    CHECK(m.type == OTK_BRIDGE_CONFIGURE && m.seq == 9u && m.len == len);
    OtkBridgeConfig dc;
    CHECK(otk_bridge_decode_configure(m.payload, m.len, &dc) && dc.step_ticks == 27000u);
    CHECK(roundtrip(OTK_BRIDGE_STOP | OTK_BRIDGE_REPLY, 65535u, NULL, 0, &p, &m));
    CHECK(m.type == 0x84u && m.seq == 65535u && m.len == 0u);
    CHECK(p.crc_errors == 0u && p.discarded == 0u);
}

static void test_parser_resynchronises(void) {
    uint8_t stream[512];
    size_t n = 0;
    /* The BL616's banner, with a sync byte in it (F-18). */
    const char *banner = "Console \xA5\x01 ready\r\n";
    memcpy(stream, banner, strlen(banner));
    n += strlen(banner);
    /* A frame cut short. */
    const uint8_t pay1[3] = { 1, 2, 3 };
    uint8_t f[64];
    const size_t f1 = otk_bridge_frame(OTK_BRIDGE_EXCHANGE, 1u, pay1, sizeof(pay1), f, sizeof(f));
    memcpy(&stream[n], f, f1 - 4u);
    n += f1 - 4u;
    /* A frame with a corrupted payload byte. */
    const size_t f2 = otk_bridge_frame(OTK_BRIDGE_EXCHANGE, 2u, pay1, sizeof(pay1), &stream[n], sizeof(stream) - n);
    stream[n + 8] ^= 0x40u;
    n += f2;
    /* Two good frames back to back. */
    const uint8_t pay3[2] = { 0xA5, 0xA5 }; /* sync bytes inside a payload */
    n += otk_bridge_frame(OTK_BRIDGE_START, 3u, pay3, sizeof(pay3), &stream[n], sizeof(stream) - n);
    n += otk_bridge_frame(OTK_BRIDGE_STOP, 4u, NULL, 0, &stream[n], sizeof(stream) - n);

    /* Fed one byte at a time. */
    OtkBridgeParser p;
    otk_bridge_parser_init(&p);
    OtkBridgeMessage m;
    uint16_t seqs[8];
    unsigned found = 0;
    for (size_t i = 0; i < n; i++) {
        size_t used = 0;
        if (otk_bridge_parse(&p, &stream[i], 1, &used, &m)) {
            seqs[found++] = m.seq;
        }
        CHECK(used == 1u);
        while (otk_bridge_parse(&p, NULL, 0, &used, &m)) { /* anything already held */
            seqs[found++] = m.seq;
        }
    }
    CHECK(found == 2u && seqs[0] == 3u && seqs[1] == 4u);
    CHECK(p.crc_errors >= 1u && p.discarded > strlen(banner));

    /* Fed in one block: the same two messages. */
    otk_bridge_parser_init(&p);
    found = 0;
    size_t at = 0, used = 0;
    while (otk_bridge_parse(&p, &stream[at], n - at, &used, &m)) {
        at += used;
        seqs[found++] = m.seq;
        CHECK(found <= 2u);
        if (found > 2u) {
            break;
        }
    }
    CHECK(found == 2u && seqs[0] == 3u && seqs[1] == 4u);
}

static void test_parser_limits(void) {
    OtkBridgeParser p;
    OtkBridgeMessage m;
    size_t used = 0;
    /* A header that claims a payload above the limit is not a header. */
    const uint8_t bad[7] = { 0xA5, 0x01, 0x01, 0x00, 0x00, 0x01, 0x04 }; /* len 1025 */
    otk_bridge_parser_init(&p);
    CHECK(!otk_bridge_parse(&p, bad, sizeof(bad), &used, &m) && used == sizeof(bad));
    CHECK(p.n < OTK_BRIDGE_HEADER_BYTES);
    /* The largest payload passes. */
    static uint8_t big[OTK_BRIDGE_MAX_PAYLOAD];
    memset(big, 0x5A, sizeof(big));
    CHECK(roundtrip(OTK_BRIDGE_EXCHANGE, 1u, big, sizeof(big), &p, &m) && m.len == OTK_BRIDGE_MAX_PAYLOAD);
    CHECK(otk_bridge_frame(OTK_BRIDGE_EXCHANGE, 1u, big, OTK_BRIDGE_MAX_PAYLOAD + 1u, big, sizeof(big)) == 0);
    /* A frame cut short, then a valid one: the valid one is found, because a
     * failed candidate is dropped one byte at a time, not as a whole frame. */
    uint8_t s2[64];
    const uint8_t pay[4] = { 9, 9, 9, 9 };
    const size_t cut = otk_bridge_frame(OTK_BRIDGE_EXCHANGE, 1u, pay, sizeof(pay), s2, sizeof(s2)) - 5u;
    const size_t good = otk_bridge_frame(OTK_BRIDGE_START, 2u, NULL, 0, &s2[cut], sizeof(s2) - cut);
    otk_bridge_parser_init(&p);
    size_t at = 0;
    bool found = false;
    while (!found && at < cut + good) {
        found = otk_bridge_parse(&p, &s2[at], cut + good - at, &used, &m);
        at += used;
    }
    CHECK(found && m.type == OTK_BRIDGE_START && m.seq == 2u);

    /* Another version is not this protocol. */
    uint8_t f[16];
    const size_t n = otk_bridge_frame(OTK_BRIDGE_STOP, 1u, NULL, 0, f, sizeof(f));
    f[1] = 2u;
    otk_bridge_parser_init(&p);
    CHECK(!otk_bridge_parse(&p, f, n, &used, &m));
}

static void test_units(void) {
    uint32_t inc = 0;
    CHECK(otk_bridge_phase_inc(2400.0, 27000000u, &inc) && inc == 208499983u); /* §3.2 */
    CHECK_NEAR(otk_bridge_rpm(inc, 27000000u), 2400.0, 1.2e-5);
    CHECK_NEAR(otk_bridge_rpm(1u, 27000000u), 1.15e-5, 0.01e-5); /* one LSB */
    CHECK(otk_bridge_phase_inc(49438.0, 27000000u, &inc));
    CHECK(!otk_bridge_phase_inc(49439.0, 27000000u, &inc)); /* above a u32 increment */
    CHECK(otk_bridge_phase_inc(0.0, 27000000u, &inc) && inc == 0u);
    CHECK(!otk_bridge_phase_inc(-1.0, 27000000u, &inc));
    CHECK(!otk_bridge_phase_inc(NAN, 27000000u, &inc));
    CHECK(!otk_bridge_phase_inc(1500.0, 0u, &inc));

    CHECK(otk_bridge_analog(0.0) == 0u && otk_bridge_analog(1.0) == 65535u);
    CHECK(otk_bridge_analog(0.5) == 32768u && otk_bridge_analog(-0.1) == 0u && otk_bridge_analog(2.0) == 65535u);
    CHECK(otk_bridge_analog(NAN) == 0u);

    uint64_t t = 0;
    CHECK(otk_bridge_extend_tick(0x00000005u, 0x100000010ull, &t) && t == 0x100000005ull);
    CHECK(otk_bridge_extend_tick(0x00000020u, 0x100000010ull, &t) && t == 0x000000020ull); /* before the wrap */
    CHECK(otk_bridge_extend_tick(0x00000010u, 0x100000010ull, &t) && t == 0x100000010ull); /* at ref */
    CHECK(otk_bridge_extend_tick(0x00000011u, 0x100000010ull, &t) && t == 0x000000011ull); /* one past ref */
    CHECK(!otk_bridge_extend_tick(0x00000020u, 0x000000010ull, &t));
}

int main(void) {
    RUN_TEST(test_crc);
    RUN_TEST(test_layout);
    RUN_TEST(test_request_and_budget);
    RUN_TEST(test_reply);
    RUN_TEST(test_control_messages);
    RUN_TEST(test_every_type_through_the_parser);
    RUN_TEST(test_parser_resynchronises);
    RUN_TEST(test_parser_limits);
    RUN_TEST(test_units);
    return check_report();
}
