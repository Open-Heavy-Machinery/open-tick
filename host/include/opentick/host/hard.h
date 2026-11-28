/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file hard.h
 * @brief The host side of the hard driver (docs/bridge-protocol.md): the
 *        serial link to a signal-level bridge, one request and its reply at
 *        a time; the clock mapping between the host and the bridge (§4); and
 *        an estimate of the link's own forwarding cycle (the BL616's, F-14),
 *        so that exchanges can be scheduled against it.
 *
 * Linux only, like the rest of host/. Times on the host are CLOCK_MONOTONIC
 * nanoseconds; times on the bridge are its ticks.
 */

#ifndef OPENTICK_HOST_HARD_H
#define OPENTICK_HOST_HARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <opentick/bridge.h>

/* ---------------------------------------------------------------- defaults (docs/bridge-protocol.md §2, §4) */

#define OTK_HARD_DEFAULT_H 16u                 /**< look-ahead, steps: held with every CPU busy (C host, BL616) */
/** Left before a forwarding instant, beyond the bytes' wire time. 600 us held without captures (2026-11-27);
 *  replies that carry captures missed the BL616's instant at 600 us and none did at 1200 us (2026-11-28,
 *  open-crank's Level 2 hard runs, docs/bridge-protocol.md §9). */
#define OTK_HARD_DEFAULT_MARGIN_NS 1200000u
#define OTK_HARD_DEFAULT_SPIN_NS 300000u       /**< spin before a scheduled send instead of sleeping */

/* ---------------------------------------------------------------- the link */

typedef struct {
    int fd;
    uint16_t seq;              /**< seq of the last request sent */
    OtkBridgeParser parser;
    uint32_t timeouts;         /**< requests with no reply in time */
    uint8_t pend[512];         /**< bytes read and not yet parsed */
    size_t npend;
    uint64_t t_pend;           /**< when the last of them was read */
} OtkHardLink;

/** @brief One request and its reply, with the host's times around it. */
typedef struct {
    uint64_t t1;                                 /**< CLOCK_MONOTONIC just before the request was written */
    uint64_t t4;                                 /**< CLOCK_MONOTONIC when the reply's last byte was read */
    uint8_t type;                                /**< the reply's type */
    uint16_t seq;
    uint16_t len;
    uint8_t payload[OTK_BRIDGE_MAX_PAYLOAD];
} OtkHardReply;

/** @brief CLOCK_MONOTONIC in nanoseconds. */
uint64_t otk_hard_now_ns(void);

/** @brief Sleeps until CLOCK_MONOTONIC reaches @p t_ns (absolute, so no drift). */
void otk_hard_sleep_until(uint64_t t_ns);

/**
 * @brief Waits until @p t_ns with little lateness: sleeps until @p spin_ns
 *        before it, then spins. An idle CPU in a deep sleep state can wake up
 *        hundreds of microseconds late, enough to miss the BL616's
 *        forwarding instant; the spin covers that. Also sets this thread's
 *        timer slack to 1 ns (Linux; no privilege needed).
 * @return how late it returned, ns
 */
uint64_t otk_hard_wait_until(uint64_t t_ns, uint64_t spin_ns);

/**
 * @brief Opens a serial device raw, 8N1, no flow control, at @p baud
 *        (one of the standard Linux rates up to 4 Mbaud).
 * @return 0, or -1 with errno set
 */
int otk_hard_open(OtkHardLink *l, const char *path, uint32_t baud);

void otk_hard_close(OtkHardLink *l);

/**
 * @brief Reads and discards whatever arrives until the line has been silent
 *        for @p silence_ns (the BL616 prints a banner after programming, F-18).
 * @return bytes discarded
 */
size_t otk_hard_drain(OtkHardLink *l, uint64_t silence_ns, uint64_t max_ns);

/**
 * @brief Sends one request and waits for its reply.
 * @param retransmit  send it again with the previous seq (§3.2)
 * @return 0 with @p r filled; 1 if no valid reply came within @p timeout_ns;
 *         -1 on an I/O error
 */
int otk_hard_transact(OtkHardLink *l, uint8_t type, const uint8_t *payload, size_t len, bool retransmit,
                      uint64_t timeout_ns, OtkHardReply *r);

/* ---------------------------------------------------------------- the clock mapping (§4) */

#define OTK_HARD_SYNC_BINS 32
#define OTK_HARD_SYNC_SLACK_NS 500000.0 /**< bins whose best delay is this much above the best are left out */

/** @brief One exchange as a timing sample: T1, T4 on the host, T2, T3 on the bridge. */
typedef struct {
    uint64_t t1, t4;  /**< host, ns */
    uint64_t t2, t3;  /**< bridge, ticks */
} OtkHardSample;

/**
 * @brief The host keeps the lowest-delay sample of every bin of host time
 *        and fits a line, offset(t) = a + b (t - t_ref), through the last
 *        OTK_HARD_SYNC_BINS of them, leaving out a bin whose best delay is
 *        more than OTK_HARD_SYNC_SLACK_NS above the lowest (a bin in which
 *        the link missed every forwarding instant). offset is bridge time
 *        minus host time.
 */
typedef struct {
    uint32_t tick_hz;
    uint64_t bin_ns;
    struct {
        bool valid;
        double t;        /**< host time of the sample's middle, ns, relative to t_ref */
        double offset;   /**< ns */
        double delay;    /**< ns: (T4 - T1) - (T3 - T2) */
        uint64_t bin;
    } best[OTK_HARD_SYNC_BINS];
    uint64_t t_ref;
    bool have_ref;
    /* the fit */
    bool fitted;
    double a, b;         /**< offset = a + b (t - t_ref), ns and ns/ns */
    double residual_max; /**< largest |offset - line| among the bins used, ns */
    double delay_min;    /**< smallest delay among them, ns */
    unsigned used;
} OtkHardSync;

void otk_hard_sync_init(OtkHardSync *s, uint32_t tick_hz, uint64_t bin_ns);

/** @brief Adds a sample; refits when it improves its bin or opens a new one. */
void otk_hard_sync_add(OtkHardSync *s, const OtkHardSample *x);

/** @brief The bound of the mapping (§4): the largest residual plus half the smallest delay, ns. */
double otk_hard_sync_bound_ns(const OtkHardSync *s);

/** @brief Bridge ticks at host time @p t_ns (needs a fit). */
double otk_hard_sync_to_bridge(const OtkHardSync *s, uint64_t t_ns);

/** @brief Host time (ns) at bridge tick @p tick (needs a fit). */
uint64_t otk_hard_sync_to_host(const OtkHardSync *s, double tick);

/* ---------------------------------------------------------------- the link's forwarding cycle */

/**
 * @brief Estimates a link's periodic forwarding cycle from the times at
 *        which replies reached the host (T4, ns). On the BL616, the
 *        bridge -> host direction is forwarded on a cycle of about 6 ms
 *        (findings F-14; the host -> bridge direction is not), so every T4
 *        falls near t0 + k P. Each gap between two replies that is close to
 *        a whole number of cycles gives an estimate of P; their median
 *        numbers the cycles, gap by gap, and t0 and P are fitted by least
 *        squares over the cycle numbers. Times in, period out, in the same
 *        unit.
 * @param p_guess  a first guess of P (6 000 000 ns on the BL616)
 * @return false with fewer than 8 usable replies
 */
bool otk_hard_cycle_fit(const uint64_t *t, size_t n, double p_guess, double *period, double *t0, double *rms);

#endif /* OPENTICK_HOST_HARD_H */
