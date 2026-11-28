/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file hard_loop.h
 * @brief The hard driver's step loop (docs/bridge-protocol.md §2, §4.1, §5,
 *        §8): the core steps on a signal-level bridge's clock, and the plant
 *        runs H steps ahead of it.
 *
 * The application computes one step at a time, exactly as with the other
 * drivers (driver.h), and hands the loop that step's setpoint:
 *
 *     OtkHardLoop hl;
 *     otk_hard_loop_open(&hl, "/dev/ttyUSB1", &cfg, err, sizeof(err));
 *     while (!done) {
 *         machine_step(...);                 // computes step k
 *         otk_hard_loop_set(&hl, &setpoint); // what the bridge applies at step k
 *         if (!otk_hard_loop_next(&hl)) {    // returns when step k + 1 is needed
 *             break;                         // aborted
 *         }
 *     }
 *     otk_hard_loop_stop(&hl);
 *
 * otk_hard_loop_next() keeps the bridge's buffer near H setpoints:
 * - before step 0 it queues the first H setpoints, sends them in one
 *   exchange and starts the bridge (§8, steps 3 and 4);
 * - after that it returns as soon as the plant is less than H steps ahead
 *   of the bridge at the next planned exchange, so the application computes
 *   another step; otherwise it waits for that exchange and sends every
 *   setpoint computed since the last one.
 * Exchanges are scheduled against the link's forwarding cycle (§4.1): one
 * per cycle, written so that the request's and the reply's bytes are on the
 * wire, and a margin is left, before the next forwarding instant.
 *
 * Step k begins at t_start + k x step_ticks on the bridge. The loop never
 * changes the application's step; the application's time of step k is
 * k x step (§1).
 *
 * The loop does not raise its scheduling priority: SCHED_FIFO is a deferred
 * improvement (the project's findings, F-33).
 *
 * Linux only, like the rest of host/.
 */

#ifndef OPENTICK_HOST_HARD_LOOP_H
#define OPENTICK_HOST_HARD_LOOP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <opentick/bridge.h>
#include <opentick/can.h>
#include <opentick/host/hard.h>
#include <opentick/host/socketcan.h>

#define OTK_HARD_MAX_H 48u           /**< the Tang Nano 20K's buffer holds 64; H + one exchange's catch-up must fit */
#define OTK_HARD_RING 128u           /**< setpoints kept for the applied check; > OTK_HARD_MAX_H + 36 */
#define OTK_HARD_CYCLE_WINDOW 400u   /**< replies the forwarding cycle is refitted over */
#define OTK_HARD_CAPTURE_QUEUE 256u  /**< captures held for the application */
#define OTK_HARD_CAN_QUEUE 64u       /**< CAN frames waiting for their step */

/* ---------------------------------------------------------------- configuration */

typedef struct {
    uint32_t baud;              /**< 3 000 000 on the BL616 */
    unsigned h;                 /**< look-ahead, steps, 1..OTK_HARD_MAX_H */
    uint64_t margin_ns;         /**< left before a forwarding instant, beyond the bytes' wire time (§4.1) */
    uint64_t spin_ns;           /**< spin before a planned send instead of sleeping */
    uint64_t timeout_ns;        /**< one attempt's wait for a reply */
    unsigned retries;           /**< retransmissions of one request before the run is aborted */
    uint64_t learn_ns;          /**< back-to-back exchanges before the fill, to learn the cycle and the mapping */
    double cycle_guess_ns;      /**< a first guess of the link's forwarding cycle (6 ms on the BL616) */
    uint64_t sync_bin_ns;       /**< the clock mapping's bins (§4) */
    uint64_t drain_silence_ns;  /**< the banner is drained until the line is this long silent (F-18) */
    bool require_fresh;         /**< refuse a bridge that is not idle, i.e. not freshly programmed (F-17) */
    OtkBridgeConfig bridge;     /**< sent as configure; its step_ticks is the step */
    uint32_t need_tick_hz;      /**< refuse another oscillator; 0: any */
    uint8_t need_analog;        /**< refuse fewer analog outputs */
    uint8_t need_capture;       /**< refuse fewer capture channels */
} OtkHardLoopConfig;

/**
 * @brief The defaults: H = OTK_HARD_DEFAULT_H, margin 600 us, spin 300 us,
 *        3 Mbaud, 100 ms per attempt with 3 retransmissions, 2 s of learning,
 *        a 6 ms cycle guess, 100 ms bins, a fresh bridge required, and a
 *        bridge configuration with a 60-2 crank wheel, the cam high over the
 *        first 360 degrees, no capture and a 100-step link timeout.
 */
void otk_hard_loop_defaults(OtkHardLoopConfig *c, uint32_t step_ticks);

/* ---------------------------------------------------------------- the run's outcome and figures */

typedef enum {
    OTK_HARD_OK = 0,       /**< no underrun, no late setpoint, every applied setpoint as sent */
    OTK_HARD_DEGRADED = 1, /**< it ran to its end, with an underrun, a late setpoint or a wrong applied setpoint */
    OTK_HARD_ABORTED = 2   /**< the link timed out or the bridge stopped answering */
} OtkHardResult;

/** @brief "ok", "degraded", "aborted". */
const char *otk_hard_result_name(OtkHardResult r);

/** @brief Why a run was aborted: the first cause the loop met. */
typedef enum {
    OTK_HARD_ABORT_NONE = 0,
    OTK_HARD_ABORT_IO,             /**< reading or writing the link failed */
    OTK_HARD_ABORT_NO_REPLY,       /**< no reply after every retransmission */
    OTK_HARD_ABORT_BAD_REPLY,      /**< a reply of the wrong type, or one that does not decode */
    OTK_HARD_ABORT_CRC,            /**< the request was damaged on the way every time */
    OTK_HARD_ABORT_BRIDGE,         /**< the bridge reports it aborted (its link timeout, §3.5) */
    OTK_HARD_ABORT_FILL,           /**< the fill before the start was not accepted whole */
    OTK_HARD_ABORT_START,          /**< the start was refused */
    OTK_HARD_ABORT_LAST_SETPOINTS, /**< at the stop, the last setpoints never got through */
    OTK_HARD_ABORT_STOP            /**< the stop got no valid reply */
} OtkHardAbort;

/** @brief "none", "io", "no-reply", "bad-reply", "crc", "bridge", "fill", "start", "last-setpoints", "stop". */
const char *otk_hard_abort_name(OtkHardAbort a);

/** @brief A capture (§3.3), extended to 64 bits and placed on the bridge's step grid. */
typedef struct {
    uint8_t channel;
    uint64_t start_tick; /**< the pulse's start, bridge ticks */
    uint32_t width;      /**< ticks */
    uint32_t step;       /**< the bridge step it started in */
} OtkHardCapture;

typedef struct {
    uint64_t exchanges;
    uint32_t steps;            /**< the bridge's last step, from stop */
    uint32_t underruns;        /**< from the bridge (saturating) */
    uint32_t late;             /**< setpoints the bridge dropped as late */
    uint32_t capture_lost;     /**< captures lost to the bridge's capture overflow */
    uint32_t seq_errors;       /**< requests refused with error.sequence */
    uint32_t crc_errors;       /**< requests answered with error.crc */
    uint32_t timeouts;         /**< attempts with no reply in time */
    uint32_t applied_checked;
    uint32_t applied_wrong;    /**< applied setpoints that were not the one sent for that step */
    uint16_t min_fill;         /**< the lowest buffer fill reported while running */
    double bound_max_ns;       /**< the clock mapping's largest bound while running (§4) */
    double bound_end_ns;       /**< its bound at the end */
    uint64_t send_late_max_ns; /**< the latest a planned send went out */
    double cycle_ns;           /**< the forwarding cycle, as last fitted */
    uint32_t capture_lag_max;  /**< the most steps the application had computed beyond a capture's step when it was handed over */
    OtkHardAbort abort_reason; /**< the first cause of an abort, or none */
    uint32_t abort_computed;   /**< steps the application had computed then */
    uint16_t abort_error;      /**< the last reply's error bits then (OTK_BRIDGE_ERR_*) */
    uint8_t abort_state;       /**< the last reply's bridge state then */
} OtkHardStats;

/* ---------------------------------------------------------------- REALTIME -> MONOTONIC (§4, §5) */

/**
 * @brief SocketCAN stamps frames on CLOCK_REALTIME; the run's host clock is
 *        CLOCK_MONOTONIC. The step between them is measured, not assumed: each
 *        sample reads REALTIME, MONOTONIC, REALTIME, and gives an offset (the
 *        middle of the two REALTIME reads minus MONOTONIC) good to half its
 *        width. The offset can move during a run (NTP slews REALTIME), so the
 *        error is the largest half width plus how far the offset moved.
 *
 *        A sample whose two REALTIME reads are more than
 *        OTK_RT_MONO_MAX_WIDTH_NS apart was interrupted (or REALTIME stepped
 *        inside it). It is read again, up to OTK_RT_MONO_TRIES times in all; one
 *        still too wide is dropped and counted in `rejected`, so a single
 *        preempted read cannot set the whole run's bound (§5).
 */
#define OTK_RT_MONO_MAX_WIDTH_NS 2000u
#define OTK_RT_MONO_TRIES 3u

typedef struct {
    bool have;
    int64_t offset_ns;     /**< REALTIME - MONOTONIC, from the latest sample */
    int64_t offset_min, offset_max;
    uint64_t half_width_max_ns;
    uint64_t samples;      /**< accepted */
    uint64_t retries;      /**< reads repeated because the previous one was too wide */
    uint64_t rejected;     /**< dropped: still too wide after every try, or REALTIME went back */
} OtkRtMono;

void otk_rt_mono_init(OtkRtMono *m);

/** @brief Takes one sample now (up to OTK_RT_MONO_TRIES reads). */
void otk_rt_mono_sample(OtkRtMono *m);

/** @brief Reads a clock, ns; `id` is CLOCK_REALTIME or CLOCK_MONOTONIC. */
typedef uint64_t (*OtkRtMonoClock)(void *ctx, int id);

/** @brief otk_rt_mono_sample() on other clocks; for tests. */
void otk_rt_mono_sample_from(OtkRtMono *m, OtkRtMonoClock read, void *ctx);

/**
 * @brief Adds a sample read elsewhere (REALTIME before, MONOTONIC, REALTIME
 *        after), with the same width rule; for tests. Returns whether it was
 *        accepted.
 */
bool otk_rt_mono_add(OtkRtMono *m, uint64_t rt_before, uint64_t mono, uint64_t rt_after);

/** @brief A REALTIME time on CLOCK_MONOTONIC. */
uint64_t otk_rt_mono_to_mono(const OtkRtMono *m, uint64_t rt_ns);

/** @brief The measured error of that step, ns. */
uint64_t otk_rt_mono_err_ns(const OtkRtMono *m);

/* ---------------------------------------------------------------- aiming at the bridge's steps (§4.1) */

/**
 * @brief The bridge's tick at a host time, estimated from the fast direction
 *        of the link (F-28): T2 + (now - T1) of the last request, corrected by
 *        the mapping's drift. Used to aim setpoints and CAN frames at a step.
 *        The published bound stays the symmetric mapping's (OtkHardSync).
 */
typedef struct {
    uint32_t tick_hz;
    uint32_t step_ticks;
    uint64_t t_start;  /**< tick at which step 0 begins */
    uint64_t t1;       /**< host time of the last request, ns */
    uint64_t t2;       /**< its arrival, bridge ticks */
    double drift;      /**< bridge minus host rate, ns/ns (OtkHardSync.b) */
} OtkHardAim;

/** @brief Bridge ticks at host time @p host_ns. */
double otk_hard_aim_tick(const OtkHardAim *a, uint64_t host_ns);

/** @brief The bridge step running at host time @p host_ns (0 before step 0). */
uint32_t otk_hard_aim_step(const OtkHardAim *a, uint64_t host_ns);

/** @brief Host time (CLOCK_MONOTONIC, ns) at which the bridge begins step @p step. */
uint64_t otk_hard_aim_step_host_ns(const OtkHardAim *a, uint32_t step);

/**
 * @brief Waits until @p host_ns (sleep, then spin) and sends @p f on @p s:
 *        a CAN frame on the bridge's clock (§5).
 * @param late_ns  set to how late it went out (may be NULL)
 * @return otk_socketcan_send()
 */
bool otk_hard_can_send_at(OtkSocketCan *s, const OtkCanFrame *f, uint64_t host_ns, uint64_t spin_ns, uint64_t *late_ns);

/* ---------------------------------------------------------------- the loop */

typedef struct {
    OtkCanFrame frame;
    uint32_t step;
} OtkHardCanItem;

/** @brief A CAN frame the loop sent at its step. */
typedef struct {
    OtkCanFrame frame;
    uint32_t step;
    uint64_t planned_ns; /**< CLOCK_MONOTONIC the bridge's step began at, as aimed */
    uint64_t sent_ns;    /**< CLOCK_MONOTONIC just after it was written */
    bool ok;             /**< otk_socketcan_send() succeeded */
} OtkHardCanSent;

typedef void (*OtkHardCanSentFn)(void *ctx, const OtkHardCanSent *sent);

typedef struct {
    OtkHardLoopConfig cfg;
    OtkHardLink link;
    OtkHardSync sync;
    OtkHardAim aim;
    OtkRtMono rt;
    OtkBridgeConfigReply caps;
    OtkHardStats stats;
    bool started, aborted;
    /* setpoints: computed (by the application), queued (sent and accepted) */
    OtkBridgeSetpoint ring[OTK_HARD_RING];
    OtkBridgeSetpoint pending;  /**< set by otk_hard_loop_set() */
    bool have_pending;
    uint32_t computed;          /**< steps the application has computed */
    uint32_t queued;            /**< setpoints the bridge accepted */
    /* the forwarding cycle */
    uint64_t t4[OTK_HARD_CYCLE_WINDOW];
    size_t n_t4;
    double cycle_p, cycle_t0;
    bool have_cycle;
    uint8_t last_n_cap;
    /* round trips, for the percentiles */
    uint64_t *rtt;
    size_t n_rtt, cap_rtt;
    /* captures for the application */
    OtkHardCapture cap[OTK_HARD_CAPTURE_QUEUE];
    size_t n_cap;
    uint32_t cap_dropped;       /**< lost because the application did not take them */
    /* CAN on the bridge's clock */
    OtkSocketCan *can;
    OtkHardCanItem can_q[OTK_HARD_CAN_QUEUE];
    size_t n_can;
    uint32_t can_overflows;
    bool can_skipped;           /**< the last cycle's exchange gave way to a CAN frame */
    OtkHardCanSentFn can_sent;
    void *can_ctx;
} OtkHardLoop;

/**
 * @brief §8, steps 1 and 2, and the learning: opens the link, drains the
 *        banner, checks that the bridge is idle (if cfg->require_fresh),
 *        configures it and checks its capabilities, then exchanges back to
 *        back for cfg->learn_ns to learn the forwarding cycle and the clock
 *        mapping.
 * @return 0; or -1 with a message in @p err (the link is closed)
 */
int otk_hard_loop_open(OtkHardLoop *l, const char *dev, const OtkHardLoopConfig *cfg, char *err, size_t err_cap);

/** @brief The setpoint of the step the application has just computed. */
void otk_hard_loop_set(OtkHardLoop *l, const OtkBridgeSetpoint *sp);

/**
 * @brief Call once after every step (after otk_hard_loop_set()). Fills,
 *        starts, then exchanges until the next step is needed; sends the CAN
 *        frames whose step has come meanwhile.
 * @return false once the run is aborted
 */
bool otk_hard_loop_next(OtkHardLoop *l);

/** @brief Steps the application has computed so far: the next one is this. */
uint32_t otk_hard_loop_computed(const OtkHardLoop *l);

/**
 * @brief Moves the captures received so far to @p out, oldest first.
 * @return how many
 */
size_t otk_hard_loop_captures(OtkHardLoop *l, OtkHardCapture *out, size_t cap);

/**
 * @brief Sends CAN frames on @p s at their step (§5). @p sent, if not NULL,
 *        is told about every frame as it goes out.
 */
void otk_hard_loop_attach_can(OtkHardLoop *l, OtkSocketCan *s, OtkHardCanSentFn sent, void *ctx);

/**
 * @brief Queues @p f to go out when the bridge begins step @p step.
 * @return false if the queue is full (counted)
 */
bool otk_hard_loop_can_at(OtkHardLoop *l, const OtkCanFrame *f, uint32_t step);

/**
 * @brief Bridge time since step 0 at host time @p host_ns (CLOCK_MONOTONIC),
 *        in ns, from the symmetric mapping (§4): what a frame log's t_bus
 *        holds with bus_clock=bridge. Needs the run started.
 * @return false before the mapping or the start
 */
bool otk_hard_loop_bridge_ns(const OtkHardLoop *l, uint64_t host_ns, int64_t *bridge_ns);

/** @brief The same for a SocketCAN kernel timestamp (CLOCK_REALTIME): REALTIME -> MONOTONIC -> bridge (§5). */
bool otk_hard_loop_bridge_ns_rt(const OtkHardLoop *l, uint64_t realtime_ns, int64_t *bridge_ns);

/**
 * @brief The bound on a t_bus from otk_hard_loop_bridge_ns_rt() over this run:
 *        the mapping's largest bound while running plus the REALTIME ->
 *        MONOTONIC step's error (§5, bus_clock_err_ns).
 */
uint64_t otk_hard_loop_bus_err_ns(const OtkHardLoop *l);

/**
 * @brief §8, step 7: sends the CAN frames still due, stops the bridge and
 *        closes the link.
 * @return the run's outcome
 */
OtkHardResult otk_hard_loop_stop(OtkHardLoop *l);

/** @brief Round trip percentile @p p (0..100) over the run, ns; 0 without exchanges. */
uint64_t otk_hard_loop_rtt_pct(const OtkHardLoop *l, double p);

/** @brief Frees what the loop allocated. Safe after a failed open. */
void otk_hard_loop_free(OtkHardLoop *l);

/**
 * @brief The DUT-to-plant delay a hard run can show (docs/bridge-protocol.md §2): how many
 *        nanoseconds of the plant's time can pass between a capture's start and the step at
 *        which the plant receives it.
 *
 *        The plant runs ahead of the bridge by up to H + 1 steps at the next planned exchange,
 *        and that exchange is up to one forwarding cycle away; a capture then waits up to one
 *        cycle for the reply that carries it, plus that reply's round trip. So
 *        (H + 1) x step + 2 x cycle + round trip. With H = 16, 1 ms steps, the BL616's 6 ms
 *        cycle and a 2.2 ms round trip: 31.2 ms.
 */
uint64_t otk_hard_dut_to_plant_ns(unsigned h, uint64_t step_ns, uint64_t cycle_ns, uint64_t rtt_ns);

/* ---------------------------------------------------------------- the budget (ROADMAP v0.4, exit criterion 3) */

/**
 * @brief What a hard run is declared to hold. A value of UINT64_MAX means
 *        "not declared": that row's `declared` and `within` are left empty.
 */
typedef struct {
    uint64_t underruns;
    uint64_t late;
    uint64_t rtt_p99_ns;
    uint64_t mapping_bound_ns;
    uint64_t realtime_err_ns;
    uint64_t can_tx_err_ns;     /**< |t_bus - t| of a CAN frame sent at its step */
} OtkHardBudget;

/** @brief Every row not declared. */
void otk_hard_budget_none(OtkHardBudget *b);

/** @brief Facts about the run that are not measured by the loop. NULL or "" leaves a key empty. */
typedef struct {
    const char *link;           /**< e.g. "bl616-3mbaud" */
    const char *usb;            /**< otk_hard_usb_path() */
    const char *bitstream_md5;
    const char *commit;
    const char *source;         /**< the program */
} OtkHardRunMeta;

/** @brief Measured CAN send error over the run (|t_bus - t| of each frame), or n = 0. */
typedef struct {
    uint64_t n;
    uint64_t p50_ns, p99_ns, max_ns;
} OtkHardCanErr;

/**
 * @brief Writes the run's budget file (docs/bridge-protocol.md, "Budget of a
 *        hard run"): a metadata line, a header, one row per quantity with what
 *        was declared and what was measured.
 * @return false if a value would break the format or the file cannot be written
 */
bool otk_hard_budget_write(FILE *fp, const OtkHardLoop *l, OtkHardResult result, const OtkHardBudget *declared,
                           const OtkHardRunMeta *meta, const OtkHardCanErr *can);

/**
 * @brief The USB path of a serial device, as sysfs names it, with its speed:
 *        "3-2@480M" (bus 3, root port 2), "3-2.1@480M" (through a hub on port
 *        2). Hubs on the way show as dots.
 * @return false if @p dev is not a USB device (then @p out is "none")
 */
bool otk_hard_usb_path(const char *dev, char *out, size_t cap);

#endif /* OPENTICK_HOST_HARD_LOOP_H */
