/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file bridge_vectors.c
 * @brief The system testbench's script and its judge, both from open-tick's
 *        codec (opentick/bridge.h), so that the gateware and the C side of
 *        the protocol cannot drift apart.
 *
 *   otk-bridge-vectors gen   <stimulus.txt>
 *   otk-bridge-vectors check <testbench-output.txt>
 *
 * `gen` writes the frames the testbench sends, and the pauses and injector
 * pulses between them. `check` walks the same scenario over what the
 * testbench logged and judges every reply against docs/bridge-protocol.md.
 *
 * Stimulus lines (sim/tb_bridge.v reads them):
 *   junk  N b0 .. bN-1      bytes sent with no reply expected
 *   send  N b0 .. bN-1      a frame; the testbench waits for one reply frame
 *   idle  C                 C clock cycles
 *   pulse CH DELAY WIDTH    injector channel CH high from DELAY cycles from now, for WIDTH
 *   edges                   log the crank and cam rising edges since the last `edges`
 *   end
 * Output lines: "sent T", "rx T N b0 ..", "pulse CH T WIDTH", "edges C M", "timeout".
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opentick/bridge.h>

#define STEP_TICKS 2700u     /* 100 us: short steps keep the simulation fast */
#define LINK_TIMEOUT 40u
#define SYNC_DELAY 2u        /* capture.v: edges are seen two ticks late */

static bool checking;
static FILE *io;
static int fails;
static uint16_t seq;
static uint64_t t_start;
static uint64_t sent_tick;
static struct { unsigned ch; uint64_t at; uint32_t width; } pulses[8];
static unsigned n_pulses;

#define EXPECT(cond, ...)                                       \
    do {                                                        \
        if (!(cond)) {                                          \
            fails++;                                            \
            fprintf(stderr, "FAIL line %d: ", __LINE__);        \
            fprintf(stderr, __VA_ARGS__);                       \
            fputc('\n', stderr);                                \
        }                                                       \
    } while (0)

/* ---------------------------------------------------------------- the setpoint of step k */
static OtkBridgeSetpoint setpoint(uint32_t k) {
    OtkBridgeSetpoint s;
    s.crank_phase_inc = 0x40000000u + k;
    s.analog[0] = (uint16_t)(k * 3u);
    s.analog[1] = (uint16_t)(k * 5u);
    s.analog[2] = (uint16_t)(0xFFFFu - k);
    s.analog[3] = 0x8000u;
    return s;
}

/* ---------------------------------------------------------------- reading the testbench's log */
static bool next_line(char *buf, size_t cap) {
    while (fgets(buf, (int)cap, io)) {
        if (buf[0] != '\n' && buf[0] != '#') {
            return true;
        }
    }
    return false;
}

/* One received frame, parsed by the codec. */
typedef struct {
    bool ok;
    uint64_t tick;
    OtkBridgeMessage m;
    uint8_t payload[OTK_BRIDGE_MAX_PAYLOAD];
} Rx;

static Rx expect_rx(int line) {
    Rx r;
    memset(&r, 0, sizeof(r));
    char buf[8192];
    if (!next_line(buf, sizeof(buf)) || strncmp(buf, "sent ", 5) != 0) {
        fails++;
        fprintf(stderr, "FAIL line %d: no 'sent' in the log\n", line);
        return r;
    }
    sent_tick = strtoull(buf + 5, NULL, 10);
    if (!next_line(buf, sizeof(buf)) || strncmp(buf, "rx ", 3) != 0) {
        fails++;
        fprintf(stderr, "FAIL line %d: no reply (%s)\n", line, buf);
        return r;
    }
    char *p = buf + 3;
    r.tick = strtoull(p, &p, 10);
    const unsigned long n = strtoul(p, &p, 10);
    uint8_t bytes[OTK_BRIDGE_FRAMING_BYTES + OTK_BRIDGE_MAX_PAYLOAD];
    for (unsigned long i = 0; i < n && i < sizeof(bytes); i++) {
        bytes[i] = (uint8_t)strtoul(p, &p, 16);
    }
    OtkBridgeParser parser;
    otk_bridge_parser_init(&parser);
    size_t used = 0;
    r.ok = otk_bridge_parse(&parser, bytes, n, &used, &r.m) && used == n;
    if (r.ok) {
        memcpy(r.payload, r.m.payload, r.m.len);
        r.m.payload = r.payload;
    }
    if (!r.ok) {
        fails++;
        fprintf(stderr, "FAIL line %d: the reply is not one valid frame (CRC or framing)\n", line);
    }
    return r;
}

/* ---------------------------------------------------------------- actions, in both modes */
static void send_frame(const uint8_t *f, size_t n) {
    fprintf(io, "send %zu", n);
    for (size_t i = 0; i < n; i++) {
        fprintf(io, " %02x", f[i]);
    }
    fputc('\n', io);
}

static void junk(const char *bytes, size_t n) {
    if (checking) {
        return;
    }
    fprintf(io, "junk %zu", n);
    for (size_t i = 0; i < n; i++) {
        fprintf(io, " %02x", (unsigned char)bytes[i]);
    }
    fputc('\n', io);
}

static void idle(unsigned cycles) {
    if (!checking) {
        fprintf(io, "idle %u\n", cycles);
    }
}

static void pulse(unsigned ch, unsigned delay, uint32_t width, int line) {
    if (!checking) {
        fprintf(io, "pulse %u %u %" PRIu32 "\n", ch, delay, width);
        return;
    }
    char buf[256];
    unsigned c = 0;
    uint64_t at = 0;
    uint32_t w = 0;
    if (!next_line(buf, sizeof(buf)) || sscanf(buf, "pulse %u %" SCNu64 " %" SCNu32, &c, &at, &w) != 3) {
        fails++;
        fprintf(stderr, "FAIL line %d: no 'pulse' in the log\n", line);
        return;
    }
    pulses[n_pulses].ch = c;
    pulses[n_pulses].at = at;
    pulses[n_pulses].width = w;
    n_pulses++;
}

static bool edges(unsigned *crank, unsigned *cam, int line) {
    if (!checking) {
        fprintf(io, "edges\n");
        return false;
    }
    char buf[256];
    if (!next_line(buf, sizeof(buf)) || sscanf(buf, "edges %u %u", crank, cam) != 2) {
        fails++;
        fprintf(stderr, "FAIL line %d: no 'edges' in the log\n", line);
        return false;
    }
    return true;
}

/* Sends a payload framed with the next seq (or `same` for a retransmission). */
static Rx exchange_raw(uint8_t type, const uint8_t *payload, size_t len, bool same, int corrupt_at, int line) {
    if (!same) {
        seq++;
    }
    uint8_t f[OTK_BRIDGE_FRAMING_BYTES + OTK_BRIDGE_MAX_PAYLOAD];
    const size_t n = otk_bridge_frame(type, seq, payload, len, f, sizeof(f));
    if (corrupt_at >= 0) {
        f[corrupt_at] ^= 0x40u;
    }
    Rx r;
    memset(&r, 0, sizeof(r));
    if (!checking) {
        send_frame(f, n);
        return r;
    }
    r = expect_rx(line);
    if (r.ok) {
        EXPECT(r.m.seq == seq, "seq %u, sent %u", r.m.seq, seq);
    }
    return r;
}

static Rx exchange(uint32_t first, uint8_t n, bool same, int line) {
    OtkBridgeRequest q;
    memset(&q, 0, sizeof(q));
    q.first_step = first;
    q.n = n;
    for (uint8_t i = 0; i < n; i++) {
        q.sp[i] = setpoint(first + i);
    }
    uint8_t p[OTK_BRIDGE_MAX_PAYLOAD];
    const size_t len = otk_bridge_encode_request(&q, p, sizeof(p));
    return exchange_raw(OTK_BRIDGE_EXCHANGE, p, len, same, -1, line);
}

static bool reply(const Rx *r, OtkBridgeReply *d, int line) {
    if (!checking || !r->ok) {
        return false;
    }
    if (r->m.type != (OTK_BRIDGE_EXCHANGE | OTK_BRIDGE_REPLY) || !otk_bridge_decode_reply(r->m.payload, r->m.len, d)) {
        fails++;
        fprintf(stderr, "FAIL line %d: not an exchange reply (type 0x%02x, len %u)\n", line, r->m.type, r->m.len);
        return false;
    }
    return true;
}

static Rx configure(uint32_t step_ticks, int line) {
    const OtkBridgeConfig c = { step_ticks, 60u, 2u, 0u, 0u, 0x80000000u, 0x3Fu, 0x3Fu, LINK_TIMEOUT };
    uint8_t p[32];
    const size_t len = otk_bridge_encode_configure(&c, p, sizeof(p));
    return exchange_raw(OTK_BRIDGE_CONFIGURE, p, len, false, -1, line);
}

static void expect_config_reply(const Rx *r, uint8_t status, int line) {
    if (!checking || !r->ok) {
        return;
    }
    OtkBridgeConfigReply d;
    EXPECT(r->m.type == 0x82u && otk_bridge_decode_configure_reply(r->m.payload, r->m.len, &d), "line %d: configure reply", line);
    if (r->m.type == 0x82u && otk_bridge_decode_configure_reply(r->m.payload, r->m.len, &d)) {
        EXPECT(d.status == status, "line %d: configure status %u, expected %u", line, d.status, status);
        EXPECT(d.tick_hz == 27000000u && d.n_capture == 6u && d.n_analog == 4u && d.analog_bits == 16u,
               "line %d: capabilities", line);
    }
}

/* The step at a tick, as the bridge counts it. */
static bool step_matches(uint32_t step, uint64_t t3) {
    if (t3 < t_start) {
        return step == 0u;
    }
    const uint64_t e = (t3 - t_start) / STEP_TICKS;
    return step == e || step + 1u == e;
}

/* ---------------------------------------------------------------- the scenario */
static void scenario(void) {
    OtkBridgeReply d;
    Rx r;
    unsigned crank = 0, cam = 0;

    /* The BL616's banner, with a sync byte and a version byte in it (F-18). */
    const char banner[] = "Console \xA5\x01 ready\r\n";
    junk(banner, sizeof(banner) - 1u);
    idle(1000);

    /* Idle: an exchange is not allowed. */
    r = exchange(0, 1, false, __LINE__);
    if (reply(&r, &d, __LINE__)) {
        EXPECT(d.state == OTK_BRIDGE_IDLE && (d.error & OTK_BRIDGE_ERR_STATE), "idle exchange: state %u error 0x%x", d.state, d.error);
    }

    r = configure(STEP_TICKS, __LINE__);
    expect_config_reply(&r, OTK_BRIDGE_STATUS_OK, __LINE__);
    r = configure(100u, __LINE__); /* below the bridge's minimum step */
    expect_config_reply(&r, OTK_BRIDGE_STATUS_UNSUPPORTED, __LINE__);

    /* Start with an empty buffer is refused. */
    r = exchange_raw(OTK_BRIDGE_START, NULL, 0, false, -1, __LINE__);
    if (checking && r.ok) {
        OtkBridgeStartReply s;
        EXPECT(r.m.type == 0x83u && otk_bridge_decode_start_reply(r.m.payload, r.m.len, &s) && s.status == 1u, "start, empty buffer");
    }

    /* Fill, then start. */
    r = exchange(0, 24, false, __LINE__);
    if (reply(&r, &d, __LINE__)) {
        EXPECT(d.state == OTK_BRIDGE_CONFIGURED && d.fill == 24u && d.error == 0u, "fill: state %u fill %u error 0x%x", d.state, d.fill, d.error);
        EXPECT(d.t2 < d.t3, "t2 before t3");
    }
    r = exchange_raw(OTK_BRIDGE_START, NULL, 0, false, -1, __LINE__);
    if (checking && r.ok) {
        OtkBridgeStartReply s;
        EXPECT(r.m.type == 0x83u && otk_bridge_decode_start_reply(r.m.payload, r.m.len, &s) && s.status == 0u, "start");
        t_start = s.t_start;
        /* step 0 begins step_ticks after the sync byte arrived: about one byte after it was sent */
        EXPECT(t_start >= sent_tick + STEP_TICKS + 80u && t_start <= sent_tick + STEP_TICKS + 130u,
               "t_start %" PRIu64 " after sending at %" PRIu64, t_start, sent_tick);
    }
    idle(10u * STEP_TICKS);

    /* Running: the applied setpoint is the one sent for the current step (F-19). */
    r = exchange(24, 8, false, __LINE__);
    if (reply(&r, &d, __LINE__)) {
        const OtkBridgeSetpoint s = setpoint(d.step);
        EXPECT(d.state == OTK_BRIDGE_RUNNING && d.error == 0u && d.underruns == 0u, "running: state %u error 0x%x underruns %u", d.state, d.error, d.underruns);
        EXPECT(step_matches(d.step, d.t3), "step %u at t3 %" PRIu64, d.step, d.t3);
        EXPECT(memcmp(&d.applied, &s, sizeof(s)) == 0, "applied setpoint of step %u", d.step);
        EXPECT(d.fill + d.step + 1u == 32u, "fill %u at step %u", d.fill, d.step);
    }
    /* A retransmission (same seq) is answered, not applied. */
    r = exchange(24, 8, true, __LINE__);
    if (reply(&r, &d, __LINE__)) {
        EXPECT(d.error == 0u && d.fill + d.step + 1u == 32u, "retransmission: fill %u step %u error 0x%x", d.fill, d.step, d.error);
    }
    /* A gap is refused. */
    r = exchange(99, 1, false, __LINE__);
    if (reply(&r, &d, __LINE__)) {
        EXPECT((d.error & OTK_BRIDGE_ERR_SEQUENCE) && d.fill + d.step + 1u == 32u, "gap: error 0x%x", d.error);
    }

    /* Two injector pulses, then they come back in the next reply. */
    (void)edges(&crank, &cam, __LINE__);
    pulse(0, 200, 500, __LINE__);
    pulse(3, 400, 1000, __LINE__);
    idle(3000);
    r = exchange(32, 4, false, __LINE__);
    if (reply(&r, &d, __LINE__)) {
        EXPECT(d.n_cap == 2u, "captures: %u", d.n_cap);
        for (unsigned i = 0; i < d.n_cap && i < 2u; i++) {
            const unsigned k = (d.cap[i].channel == 0u) ? 0u : 1u;
            EXPECT(d.cap[i].channel == pulses[k].ch && d.cap[i].width == pulses[k].width,
                   "capture %u: channel %u width %u", i, d.cap[i].channel, d.cap[i].width);
            uint64_t start = 0;
            EXPECT(otk_bridge_extend_tick(d.cap[i].start, d.t3, &start) && start == pulses[k].at + SYNC_DELAY,
                   "capture %u: start %" PRIu64 ", pulse at %" PRIu64, i, start, pulses[k].at);
        }
        EXPECT(d.error == 0u && d.fill + d.step + 1u == 36u, "after captures: fill %u step %u", d.fill, d.step);
    }
    if (edges(&crank, &cam, __LINE__)) {
        EXPECT(crank > 0u, "the crank turns while running");
    }

    /* Let the buffer run dry: underruns, then setpoints for steps that have begun are late. */
    idle(25u * STEP_TICKS);
    r = exchange(36, 4, false, __LINE__);
    if (reply(&r, &d, __LINE__)) {
        EXPECT(d.underruns > 0u && (d.error & OTK_BRIDGE_ERR_LATE) && d.error_count >= 4u && d.state == OTK_BRIDGE_RUNNING,
               "late: underruns %u error 0x%x count %u", d.underruns, d.error, d.error_count);
        const OtkBridgeSetpoint s = setpoint(35u);
        EXPECT(memcmp(&d.applied, &s, sizeof(s)) == 0, "an underrun holds the last setpoint (step 35's)");
    }

    /* A corrupted frame gets an exchange reply with error.crc and the seq as sent. */
    {
        OtkBridgeRequest q;
        memset(&q, 0, sizeof(q));
        q.first_step = 40u;
        q.n = 1;
        q.sp[0] = setpoint(40u);
        uint8_t p[64];
        const size_t len = otk_bridge_encode_request(&q, p, sizeof(p));
        r = exchange_raw(OTK_BRIDGE_EXCHANGE, p, len, false, 10, __LINE__);
        if (reply(&r, &d, __LINE__)) {
            EXPECT((d.error & OTK_BRIDGE_ERR_CRC) != 0u, "corrupted: error 0x%x", d.error);
        }
    }
    /* An unknown type. */
    r = exchange_raw(0x07u, NULL, 0, false, -1, __LINE__);
    if (reply(&r, &d, __LINE__)) {
        EXPECT((d.error & OTK_BRIDGE_ERR_STATE) != 0u, "unknown type: error 0x%x", d.error);
    }

    /* Silence: the link times out and the run is aborted, outputs at rest. */
    idle((LINK_TIMEOUT + 5u) * STEP_TICKS);
    r = exchange(40, 1, false, __LINE__);
    if (reply(&r, &d, __LINE__)) {
        EXPECT(d.state == OTK_BRIDGE_ABORTED && (d.error & OTK_BRIDGE_ERR_ABORTED) && (d.error & OTK_BRIDGE_ERR_STATE),
               "aborted: state %u error 0x%x", d.state, d.error);
    }
    (void)edges(&crank, &cam, __LINE__);
    idle(20000);
    if (edges(&crank, &cam, __LINE__)) {
        EXPECT(crank == 0u && cam == 0u, "no edges at rest: crank %u cam %u", crank, cam);
    }

    /* A new run, then stop. */
    r = configure(STEP_TICKS, __LINE__);
    expect_config_reply(&r, OTK_BRIDGE_STATUS_OK, __LINE__);
    r = exchange(0, 2, false, __LINE__);
    if (reply(&r, &d, __LINE__)) {
        EXPECT(d.state == OTK_BRIDGE_CONFIGURED && d.fill == 2u && d.error == 0u && d.underruns == 0u,
               "reconfigured: state %u fill %u error 0x%x", d.state, d.fill, d.error);
    }
    r = exchange_raw(OTK_BRIDGE_START, NULL, 0, false, -1, __LINE__);
    if (checking && r.ok) {
        OtkBridgeStartReply s;
        EXPECT(r.m.type == 0x83u && otk_bridge_decode_start_reply(r.m.payload, r.m.len, &s) && s.status == 0u, "second start");
    }
    idle(4u * STEP_TICKS);
    r = exchange_raw(OTK_BRIDGE_STOP, NULL, 0, false, -1, __LINE__);
    if (checking && r.ok) {
        OtkBridgeStopReply s;
        EXPECT(r.m.type == 0x84u && otk_bridge_decode_stop_reply(r.m.payload, r.m.len, &s) && s.status == 0u &&
               s.step >= 2u && s.underruns >= 1u, "stop");
    }
    r = exchange(2, 1, false, __LINE__);
    if (reply(&r, &d, __LINE__)) {
        EXPECT(d.state == OTK_BRIDGE_STOPPED && (d.error & OTK_BRIDGE_ERR_STATE), "stopped: state %u error 0x%x", d.state, d.error);
    }
    r = exchange_raw(OTK_BRIDGE_STOP, NULL, 0, false, -1, __LINE__);
    if (checking && r.ok) {
        OtkBridgeStopReply s;
        EXPECT(r.m.type == 0x84u && otk_bridge_decode_stop_reply(r.m.payload, r.m.len, &s) && s.status == 1u, "stop when stopped");
    }
    if (!checking) {
        fprintf(io, "end\n");
    }
}

int main(int argc, char **argv) {
    if (argc != 3 || (strcmp(argv[1], "gen") != 0 && strcmp(argv[1], "check") != 0)) {
        fprintf(stderr, "usage: otk-bridge-vectors gen <stimulus> | check <testbench-output>\n");
        return 2;
    }
    checking = strcmp(argv[1], "check") == 0;
    io = fopen(argv[2], checking ? "r" : "w");
    if (!io) {
        perror(argv[2]);
        return 2;
    }
    scenario();
    if (checking) {
        char buf[256];
        if (next_line(buf, sizeof(buf))) {
            fails++;
            fprintf(stderr, "FAIL: more in the log than the scenario expects: %s", buf);
        }
        printf("otk-bridge-vectors: %s (%d failures)\n", fails == 0 ? "PASS" : "FAIL", fails);
    }
    fclose(io);
    return fails == 0 ? 0 : 1;
}
