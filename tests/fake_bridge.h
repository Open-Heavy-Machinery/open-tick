/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file fake_bridge.h
 * @brief A bridge in a thread, on a pseudo-terminal, for the hard driver's
 *        tests without a board. It follows docs/bridge-protocol.md v1 closely
 *        enough for the host: the five states, a step engine on its own tick
 *        (CLOCK_MONOTONIC x 27 MHz, plus an offset and a drift), the setpoint
 *        buffer with underruns and late setpoints, retransmissions, the link
 *        timeout, captures, and replies held to a forwarding cycle as the
 *        BL616 does (F-27). Faults are switched on by the test.
 */

#ifndef OTK_TEST_FAKE_BRIDGE_H
#define OTK_TEST_FAKE_BRIDGE_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include <opentick/bridge.h>

#define FAKE_MAX_STEPS 4096u
#define FAKE_BUF 64u

typedef struct {
    /* set before fake_start() */
    double cycle_ns;          /**< replies wait for the next instant of this cycle; 0: at once */
    double drift;             /**< the fake's tick runs (1 + drift) fast */
    uint8_t initial_state;    /**< OTK_BRIDGE_IDLE for a fresh bitstream */
    uint8_t n_analog;
    uint8_t n_capture;
    /* faults, by step; UINT32_MAX: off */
    uint32_t drop_setpoint_at; /**< the setpoint for this step is lost inside the bridge: an underrun */
    uint32_t report_late_at;   /**< one reply after this step reports one late setpoint */
    uint32_t silent_from;      /**< from this step on, no reply */
    uint32_t capture_at;       /**< a capture on channel 2 starts 1000 ticks into this step */
    uint32_t wrong_applied_from; /**< from this step on, the bridge applies analog[3] off by one (F-19) */

    /* the thread */
    int fd;
    pthread_t th;
    atomic_bool quit;
    uint64_t base_mono;
    double offset_ticks;

    /* the bridge: the thread's own; the test reads it after fake_stop() */
    uint8_t state;
    uint16_t last_seq;
    bool have_seq;
    uint32_t step_ticks;
    uint16_t link_timeout_steps;
    uint64_t t_start;
    uint64_t last_rx_tick;
    uint32_t next_step;        /**< the next step to begin */
    uint32_t cur_step;
    bool begun;
    OtkBridgeSetpoint buf[FAKE_BUF];
    uint32_t buf_step[FAKE_BUF];
    unsigned nbuf;
    uint32_t last_queued;      /**< last step queued + 1 */
    OtkBridgeSetpoint applied_now;
    uint32_t underruns;
    uint16_t pending_err, pending_count;
    bool late_reported, capture_done;
    OtkBridgeCapture caps[8];
    unsigned ncaps;
    /* what happened, for the test */
    OtkBridgeSetpoint applied[FAKE_MAX_STEPS];
    bool applied_set[FAKE_MAX_STEPS];
    uint32_t stop_step;
    bool stopped;
    uint32_t requests;
} FakeBridge;

/** @brief Defaults: a 2 ms cycle, no drift, idle, 4 analog, 6 capture, no fault. */
void fake_init(FakeBridge *f);

/** @brief Opens a pty and starts the thread. @return the slave's path, or NULL */
const char *fake_start(FakeBridge *f, char *name, size_t cap);

/** @brief The fake's tick at host time @p mono_ns. */
uint64_t fake_tick(const FakeBridge *f, uint64_t mono_ns);

/** @brief Stops the thread and closes the pty. */
void fake_stop(FakeBridge *f);

#endif /* OTK_TEST_FAKE_BRIDGE_H */
