/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file bridge.c
 * @brief Host <-> bridge protocol v1 codec (opentick/bridge.h, docs/bridge-protocol.md).
 */

#include <opentick/bridge.h>

#include <string.h>

#define PHASE_FULL 281474976710656.0 /* 2^48: one 720-degree cycle of the rotary phase */

/* ---------------------------------------------------------------- bytes */

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4u; i++) {
        p[i] = (uint8_t)((v >> (8u * i)) & 0xFFu);
    }
}

static void put64(uint8_t *p, uint64_t v) {
    for (unsigned i = 0; i < 8u; i++) {
        p[i] = (uint8_t)((v >> (8u * i)) & 0xFFu);
    }
}

static uint16_t get16(const uint8_t *p) {
    return (uint16_t)(p[0] | (uint16_t)(p[1] << 8));
}

static uint32_t get32(const uint8_t *p) {
    uint32_t v = 0;
    for (unsigned i = 0; i < 4u; i++) {
        v |= (uint32_t)p[i] << (8u * i);
    }
    return v;
}

static uint64_t get64(const uint8_t *p) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8u; i++) {
        v |= (uint64_t)p[i] << (8u * i);
    }
    return v;
}

/* ---------------------------------------------------------------- CRC and framing */

uint16_t otk_bridge_crc16(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i] << 8;
        for (unsigned b = 0; b < 8u; b++) {
            crc = ((crc & 0x8000u) != 0u) ? ((crc << 1) ^ 0x1021u) : (crc << 1);
            crc &= 0xFFFFu;
        }
    }
    return (uint16_t)crc;
}

size_t otk_bridge_frame(uint8_t type, uint16_t seq, const uint8_t *payload, size_t len, uint8_t *out, size_t cap) {
    const size_t total = OTK_BRIDGE_FRAMING_BYTES + len;
    if (len > OTK_BRIDGE_MAX_PAYLOAD || cap < total) {
        return 0;
    }
    out[0] = OTK_BRIDGE_SYNC;
    out[1] = OTK_BRIDGE_VERSION;
    out[2] = type;
    put16(&out[3], seq);
    put16(&out[5], (uint16_t)len);
    if (len > 0) {
        memcpy(&out[OTK_BRIDGE_HEADER_BYTES], payload, len);
    }
    put16(&out[OTK_BRIDGE_HEADER_BYTES + len], otk_bridge_crc16(&out[1], OTK_BRIDGE_HEADER_BYTES - 1u + len));
    return total;
}

/* ---------------------------------------------------------------- payloads */

static void put_setpoint(uint8_t *p, const OtkBridgeSetpoint *s) {
    put32(p, s->crank_phase_inc);
    for (unsigned k = 0; k < OTK_BRIDGE_ANALOG; k++) {
        put16(&p[4u + 2u * k], s->analog[k]);
    }
}

static void get_setpoint(const uint8_t *p, OtkBridgeSetpoint *s) {
    s->crank_phase_inc = get32(p);
    for (unsigned k = 0; k < OTK_BRIDGE_ANALOG; k++) {
        s->analog[k] = get16(&p[4u + 2u * k]);
    }
}

size_t otk_bridge_encode_request(const OtkBridgeRequest *m, uint8_t *out, size_t cap) {
    const size_t len = 5u + (size_t)m->n * OTK_BRIDGE_SETPOINT_BYTES;
    if (m->n > OTK_BRIDGE_MAX_SETPOINTS || cap < len) {
        return 0;
    }
    put32(out, m->first_step);
    out[4] = m->n;
    for (size_t i = 0; i < m->n; i++) {
        put_setpoint(&out[5u + i * OTK_BRIDGE_SETPOINT_BYTES], &m->sp[i]);
    }
    return len;
}

bool otk_bridge_decode_request(const uint8_t *p, size_t len, OtkBridgeRequest *m) {
    if (len < 5u || p[4] > OTK_BRIDGE_MAX_SETPOINTS || len != 5u + (size_t)p[4] * OTK_BRIDGE_SETPOINT_BYTES) {
        return false;
    }
    m->first_step = get32(p);
    m->n = p[4];
    for (size_t i = 0; i < m->n; i++) {
        get_setpoint(&p[5u + i * OTK_BRIDGE_SETPOINT_BYTES], &m->sp[i]);
    }
    return true;
}

size_t otk_bridge_encode_reply(const OtkBridgeReply *m, uint8_t *out, size_t cap) {
    const size_t len = OTK_BRIDGE_REPLY_FIXED_BYTES + (size_t)m->n_cap * OTK_BRIDGE_CAPTURE_BYTES;
    if (m->n_cap > OTK_BRIDGE_MAX_CAPTURES || cap < len) {
        return 0;
    }
    put64(&out[0], m->t2);
    put64(&out[8], m->t3);
    put32(&out[16], m->step);
    out[20] = m->state;
    put16(&out[21], m->fill);
    put32(&out[23], m->underruns);
    put16(&out[27], m->error);
    put16(&out[29], m->error_count);
    put_setpoint(&out[31], &m->applied);
    out[43] = m->n_cap;
    for (size_t i = 0; i < m->n_cap; i++) {
        uint8_t *c = &out[OTK_BRIDGE_REPLY_FIXED_BYTES + i * OTK_BRIDGE_CAPTURE_BYTES];
        c[0] = m->cap[i].channel;
        put32(&c[1], m->cap[i].start);
        put32(&c[5], m->cap[i].width);
    }
    return len;
}

bool otk_bridge_decode_reply(const uint8_t *p, size_t len, OtkBridgeReply *m) {
    if (len < OTK_BRIDGE_REPLY_FIXED_BYTES || p[43] > OTK_BRIDGE_MAX_CAPTURES ||
        len != OTK_BRIDGE_REPLY_FIXED_BYTES + (size_t)p[43] * OTK_BRIDGE_CAPTURE_BYTES) {
        return false;
    }
    m->t2 = get64(&p[0]);
    m->t3 = get64(&p[8]);
    m->step = get32(&p[16]);
    m->state = p[20];
    m->fill = get16(&p[21]);
    m->underruns = get32(&p[23]);
    m->error = get16(&p[27]);
    m->error_count = get16(&p[29]);
    get_setpoint(&p[31], &m->applied);
    m->n_cap = p[43];
    for (size_t i = 0; i < m->n_cap; i++) {
        const uint8_t *c = &p[OTK_BRIDGE_REPLY_FIXED_BYTES + i * OTK_BRIDGE_CAPTURE_BYTES];
        m->cap[i].channel = c[0];
        m->cap[i].start = get32(&c[1]);
        m->cap[i].width = get32(&c[5]);
    }
    return true;
}

size_t otk_bridge_encode_configure(const OtkBridgeConfig *m, uint8_t *out, size_t cap) {
    if (cap < OTK_BRIDGE_CONFIGURE_BYTES) {
        return 0;
    }
    put32(&out[0], m->step_ticks);
    out[4] = m->crank_teeth;
    out[5] = m->crank_missing;
    put32(&out[6], m->crank_offset);
    put32(&out[10], m->cam_start);
    put32(&out[14], m->cam_end);
    out[18] = m->capture_mask;
    out[19] = m->capture_polarity;
    put16(&out[20], m->link_timeout_steps);
    return OTK_BRIDGE_CONFIGURE_BYTES;
}

bool otk_bridge_decode_configure(const uint8_t *p, size_t len, OtkBridgeConfig *m) {
    if (len != OTK_BRIDGE_CONFIGURE_BYTES) {
        return false;
    }
    m->step_ticks = get32(&p[0]);
    m->crank_teeth = p[4];
    m->crank_missing = p[5];
    m->crank_offset = get32(&p[6]);
    m->cam_start = get32(&p[10]);
    m->cam_end = get32(&p[14]);
    m->capture_mask = p[18];
    m->capture_polarity = p[19];
    m->link_timeout_steps = get16(&p[20]);
    return true;
}

size_t otk_bridge_encode_configure_reply(const OtkBridgeConfigReply *m, uint8_t *out, size_t cap) {
    if (cap < 8u) {
        return 0;
    }
    out[0] = m->status;
    put32(&out[1], m->tick_hz);
    out[5] = m->n_capture;
    out[6] = m->n_analog;
    out[7] = m->analog_bits;
    return 8u;
}

bool otk_bridge_decode_configure_reply(const uint8_t *p, size_t len, OtkBridgeConfigReply *m) {
    if (len != 8u) {
        return false;
    }
    m->status = p[0];
    m->tick_hz = get32(&p[1]);
    m->n_capture = p[5];
    m->n_analog = p[6];
    m->analog_bits = p[7];
    return true;
}

size_t otk_bridge_encode_start_reply(const OtkBridgeStartReply *m, uint8_t *out, size_t cap) {
    if (cap < 9u) {
        return 0;
    }
    out[0] = m->status;
    put64(&out[1], m->t_start);
    return 9u;
}

bool otk_bridge_decode_start_reply(const uint8_t *p, size_t len, OtkBridgeStartReply *m) {
    if (len != 9u) {
        return false;
    }
    m->status = p[0];
    m->t_start = get64(&p[1]);
    return true;
}

size_t otk_bridge_encode_stop_reply(const OtkBridgeStopReply *m, uint8_t *out, size_t cap) {
    if (cap < 9u) {
        return 0;
    }
    out[0] = m->status;
    put32(&out[1], m->step);
    put32(&out[5], m->underruns);
    return 9u;
}

bool otk_bridge_decode_stop_reply(const uint8_t *p, size_t len, OtkBridgeStopReply *m) {
    if (len != 9u) {
        return false;
    }
    m->status = p[0];
    m->step = get32(&p[1]);
    m->underruns = get32(&p[5]);
    return true;
}

/* ---------------------------------------------------------------- parser */

void otk_bridge_parser_init(OtkBridgeParser *p) {
    memset(p, 0, sizeof(*p));
}

static void drop(OtkBridgeParser *p, size_t k) {
    memmove(p->buf, &p->buf[k], p->n - k);
    p->n -= k;
}

/* Looks for a message in the bytes held. */
static bool scan(OtkBridgeParser *p, OtkBridgeMessage *msg) {
    for (;;) {
        size_t k = 0;
        while (k < p->n && p->buf[k] != OTK_BRIDGE_SYNC) {
            k++;
        }
        if (k > 0) {
            p->discarded += (uint32_t)k;
            drop(p, k);
        }
        if (p->n < OTK_BRIDGE_HEADER_BYTES) {
            return false;
        }
        const uint16_t len = get16(&p->buf[5]);
        if (p->buf[1] != OTK_BRIDGE_VERSION || len > OTK_BRIDGE_MAX_PAYLOAD) {
            p->discarded++; /* not a header: this sync byte was data */
            drop(p, 1);
            continue;
        }
        const size_t need = OTK_BRIDGE_FRAMING_BYTES + len;
        if (p->n < need) {
            return false;
        }
        const uint16_t crc = get16(&p->buf[OTK_BRIDGE_HEADER_BYTES + len]);
        if (crc != otk_bridge_crc16(&p->buf[1], OTK_BRIDGE_HEADER_BYTES - 1u + len)) {
            p->crc_errors++;
            p->discarded++;
            drop(p, 1); /* a valid frame may start inside this one */
            continue;
        }
        msg->type = p->buf[2];
        msg->seq = get16(&p->buf[3]);
        msg->len = len;
        msg->payload = &p->buf[OTK_BRIDGE_HEADER_BYTES];
        p->pending_drop = need;
        return true;
    }
}

bool otk_bridge_parse(OtkBridgeParser *p, const uint8_t *data, size_t len, size_t *used, OtkBridgeMessage *msg) {
    if (p->pending_drop > 0) {
        drop(p, p->pending_drop);
        p->pending_drop = 0;
    }
    *used = 0;
    if (scan(p, msg)) {
        return true; /* already held, from an earlier resynchronisation */
    }
    while (*used < len) {
        /* Never full here: scan() keeps at most one candidate frame minus one byte. */
        p->buf[p->n++] = data[(*used)++];
        if (scan(p, msg)) {
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------- units */

bool otk_bridge_phase_inc(double rpm, uint32_t tick_hz, uint32_t *inc) {
    if (!(rpm >= 0.0) || tick_hz == 0u) {
        return false;
    }
    const double v = rpm / 120.0 * PHASE_FULL / (double)tick_hz + 0.5; /* >= 0.5: truncation rounds */
    if (!(v < 4294967296.0)) {
        return false;
    }
    *inc = (uint32_t)v;
    return true;
}

double otk_bridge_rpm(uint32_t inc, uint32_t tick_hz) {
    return (double)inc * (double)tick_hz * 120.0 / PHASE_FULL;
}

uint16_t otk_bridge_analog(double fraction) {
    if (!(fraction > 0.0)) {
        return 0;
    }
    if (fraction >= 1.0) {
        return 65535u;
    }
    return (uint16_t)(fraction * 65535.0 + 0.5); /* positive: truncation rounds */
}

bool otk_bridge_extend_tick(uint32_t low, uint64_t ref, uint64_t *tick) {
    uint64_t t = (ref & ~(uint64_t)0xFFFFFFFFu) | low;
    if (t > ref) {
        if (ref < ((uint64_t)1 << 32)) {
            return false;
        }
        t -= (uint64_t)1 << 32;
    }
    *tick = t;
    return true;
}
