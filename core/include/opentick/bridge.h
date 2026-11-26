/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file bridge.h
 * @brief The host <-> bridge protocol, version 1: framing, CRC, the payload
 *        of every message, a parser that resynchronises, and the unit
 *        conversions. The contract is docs/bridge-protocol.md; this file
 *        follows its section numbers.
 *
 * Pure ISO C: no I/O, no clock, no allocation. Both sides are here, so that
 * the host (which encodes requests and decodes replies), a bridge simulator
 * and the tests can share one codec. Every integer on the wire is
 * little-endian and unsigned.
 *
 *   frame = sync(0xA5) version(1) type seq:u16 len:u16 payload[len] crc:u16
 *   crc   = CRC-16/IBM-3740 (CCITT-FALSE) over version .. end of payload
 *
 * Encoders write a payload into a caller's buffer and return its length, or
 * 0 if the message is invalid or the buffer too small. Decoders check the
 * payload's length exactly and return false on any mismatch.
 */

#ifndef OPENTICK_BRIDGE_H
#define OPENTICK_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OTK_BRIDGE_SYNC 0xA5u
#define OTK_BRIDGE_VERSION 1u
#define OTK_BRIDGE_HEADER_BYTES 7u                                   /**< sync, version, type, seq, len */
#define OTK_BRIDGE_FRAMING_BYTES (OTK_BRIDGE_HEADER_BYTES + 2u)      /**< plus the CRC */
#define OTK_BRIDGE_MAX_PAYLOAD 1024u                                 /**< the parser's limit, not the link's budget */
#define OTK_BRIDGE_BUDGET_BL616 448u                                 /**< bytes per direction per exchange (§3) */
#define OTK_BRIDGE_ANALOG 4u                                         /**< analog outputs per setpoint */
#define OTK_BRIDGE_SETPOINT_BYTES 12u
#define OTK_BRIDGE_MAX_SETPOINTS 36u                                 /**< the most that fit the BL616 budget */
#define OTK_BRIDGE_CAPTURE_BYTES 9u
#define OTK_BRIDGE_MAX_CAPTURES 32u
#define OTK_BRIDGE_REPLY_FIXED_BYTES 44u                             /**< §3.3 up to and including n_cap */
#define OTK_BRIDGE_CONFIGURE_BYTES 22u
#define OTK_BRIDGE_PHASE_BITS 48                                     /**< the rotary oscillator: 2^48 = 720 degrees */

/** @brief Message types (§3.1). A reply is its request's type + OTK_BRIDGE_REPLY. */
typedef enum {
    OTK_BRIDGE_EXCHANGE = 0x01,
    OTK_BRIDGE_CONFIGURE = 0x02,
    OTK_BRIDGE_START = 0x03,
    OTK_BRIDGE_STOP = 0x04,
    OTK_BRIDGE_REPLY = 0x80
} OtkBridgeType;

/** @brief The bridge's states (§3.4). */
typedef enum {
    OTK_BRIDGE_IDLE = 0,
    OTK_BRIDGE_CONFIGURED = 1,
    OTK_BRIDGE_RUNNING = 2,
    OTK_BRIDGE_STOPPED = 3,
    OTK_BRIDGE_ABORTED = 4
} OtkBridgeState;

/** @brief Bits of a reply's error field (§3.3). */
enum {
    OTK_BRIDGE_ERR_CRC = 1u << 0,
    OTK_BRIDGE_ERR_SEQUENCE = 1u << 1,
    OTK_BRIDGE_ERR_LATE = 1u << 2,
    OTK_BRIDGE_ERR_CAPTURE_OVERFLOW = 1u << 3,
    OTK_BRIDGE_ERR_ABORTED = 1u << 4,
    OTK_BRIDGE_ERR_STATE = 1u << 5
};

/** @brief Status byte of the configure, start and stop replies (§3.4). */
enum {
    OTK_BRIDGE_STATUS_OK = 0,
    OTK_BRIDGE_STATUS_WRONG_STATE = 1,
    OTK_BRIDGE_STATUS_UNSUPPORTED = 2
};

/** @brief One step's setpoint (§3.2). */
typedef struct {
    uint32_t crank_phase_inc;             /**< added to the 48-bit phase every tick */
    uint16_t analog[OTK_BRIDGE_ANALOG];   /**< 0 .. 65535 = 0 .. full scale */
} OtkBridgeSetpoint;

/** @brief Exchange request (§3.2). */
typedef struct {
    uint32_t first_step;
    uint8_t n;
    OtkBridgeSetpoint sp[OTK_BRIDGE_MAX_SETPOINTS];
} OtkBridgeRequest;

/** @brief One injector capture (§3.3). */
typedef struct {
    uint8_t channel;
    uint32_t start;   /**< low 32 bits of the tick; otk_bridge_extend_tick() */
    uint32_t width;   /**< ticks */
} OtkBridgeCapture;

/** @brief Exchange reply (§3.3). */
typedef struct {
    uint64_t t2;              /**< tick at which the request's first byte arrived */
    uint64_t t3;              /**< tick at which this reply was queued */
    uint32_t step;
    uint8_t state;            /**< OtkBridgeState */
    uint16_t fill;
    uint32_t underruns;       /**< saturating */
    uint16_t error;           /**< OTK_BRIDGE_ERR_* */
    uint16_t error_count;
    OtkBridgeSetpoint applied;
    uint8_t n_cap;
    OtkBridgeCapture cap[OTK_BRIDGE_MAX_CAPTURES];
} OtkBridgeReply;

/** @brief Configure (§3.4). */
typedef struct {
    uint32_t step_ticks;
    uint8_t crank_teeth;
    uint8_t crank_missing;
    uint32_t crank_offset;          /**< top 32 bits of the phase, 2^32 = 720 degrees */
    uint32_t cam_start;
    uint32_t cam_end;
    uint8_t capture_mask;
    uint8_t capture_polarity;
    uint16_t link_timeout_steps;
} OtkBridgeConfig;

/** @brief Configure's reply (§3.4): what the bridge has. */
typedef struct {
    uint8_t status;
    uint32_t tick_hz;
    uint8_t n_capture;
    uint8_t n_analog;
    uint8_t analog_bits;
} OtkBridgeConfigReply;

/** @brief Start's reply (§3.4). */
typedef struct {
    uint8_t status;
    uint64_t t_start;   /**< tick at which step 0 begins */
} OtkBridgeStartReply;

/** @brief Stop's reply (§3.4). */
typedef struct {
    uint8_t status;
    uint32_t step;
    uint32_t underruns;
} OtkBridgeStopReply;

/** @brief CRC-16/IBM-3740: polynomial 0x1021, initial 0xFFFF, no reflection, no final XOR. */
uint16_t otk_bridge_crc16(const uint8_t *data, size_t len);

/**
 * @brief Frames a payload: header, payload, CRC.
 * @return bytes written to @p out, or 0 if @p len exceeds OTK_BRIDGE_MAX_PAYLOAD
 *         or @p cap is too small
 */
size_t otk_bridge_frame(uint8_t type, uint16_t seq, const uint8_t *payload, size_t len, uint8_t *out, size_t cap);

size_t otk_bridge_encode_request(const OtkBridgeRequest *m, uint8_t *out, size_t cap);
bool otk_bridge_decode_request(const uint8_t *p, size_t len, OtkBridgeRequest *m);
size_t otk_bridge_encode_reply(const OtkBridgeReply *m, uint8_t *out, size_t cap);
bool otk_bridge_decode_reply(const uint8_t *p, size_t len, OtkBridgeReply *m);
size_t otk_bridge_encode_configure(const OtkBridgeConfig *m, uint8_t *out, size_t cap);
bool otk_bridge_decode_configure(const uint8_t *p, size_t len, OtkBridgeConfig *m);
size_t otk_bridge_encode_configure_reply(const OtkBridgeConfigReply *m, uint8_t *out, size_t cap);
bool otk_bridge_decode_configure_reply(const uint8_t *p, size_t len, OtkBridgeConfigReply *m);
size_t otk_bridge_encode_start_reply(const OtkBridgeStartReply *m, uint8_t *out, size_t cap);
bool otk_bridge_decode_start_reply(const uint8_t *p, size_t len, OtkBridgeStartReply *m);
size_t otk_bridge_encode_stop_reply(const OtkBridgeStopReply *m, uint8_t *out, size_t cap);
bool otk_bridge_decode_stop_reply(const uint8_t *p, size_t len, OtkBridgeStopReply *m);

/** @brief One message found by the parser. @p payload points into the parser and stays valid until its next call. */
typedef struct {
    uint8_t type;
    uint16_t seq;
    uint16_t len;
    const uint8_t *payload;
} OtkBridgeMessage;

/** @brief A stream parser that resynchronises on the sync byte (§3.1). */
typedef struct {
    uint8_t buf[OTK_BRIDGE_FRAMING_BYTES + OTK_BRIDGE_MAX_PAYLOAD];
    size_t n;             /**< bytes held */
    size_t pending_drop;  /**< bytes of the message last returned, dropped on the next call */
    uint32_t crc_errors;  /**< candidate frames whose CRC failed */
    uint32_t discarded;   /**< bytes skipped while looking for a frame */
} OtkBridgeParser;

void otk_bridge_parser_init(OtkBridgeParser *p);

/**
 * @brief Feeds bytes until one complete message whose CRC checks is found.
 *        Junk before it (the BL616's banner, a frame cut short) is skipped,
 *        and a frame whose CRC fails is dropped one byte at a time, so a
 *        valid frame inside it is still found. Call it again with the rest
 *        of the data, or with @p len 0, to get the next message.
 * @param used  set to the bytes of @p data consumed
 * @return true if @p msg holds a message
 */
bool otk_bridge_parse(OtkBridgeParser *p, const uint8_t *data, size_t len, size_t *used, OtkBridgeMessage *msg);

/**
 * @brief The phase increment for a speed (§3.2): round(rpm / 120 * 2^48 / tick_hz).
 * @return false if @p rpm is negative, not a number, or above what a u32 increment can reach
 */
bool otk_bridge_phase_inc(double rpm, uint32_t tick_hz, uint32_t *inc);

/** @brief The speed of a phase increment, in rpm. */
double otk_bridge_rpm(uint32_t inc, uint32_t tick_hz);

/** @brief An analog setpoint from a fraction of full scale, clamped to 0..1 (NaN gives 0). */
uint16_t otk_bridge_analog(double fraction);

/**
 * @brief The 64-bit tick of a capture's start (§3.3): the latest tick at or
 *        before @p ref whose low 32 bits are @p low.
 * @return false if there is none (@p ref below 2^32 and @p low above it)
 */
bool otk_bridge_extend_tick(uint32_t low, uint64_t ref, uint64_t *tick);

#endif /* OPENTICK_BRIDGE_H */
