/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file framelog.h
 * @brief Frame log: every CAN frame of a run, with two times.
 *
 * The application writes one row per frame it puts on its bus or takes off
 * it, through a link or not (docs/frame-log-format.md is the contract):
 *
 *     OtkFrameLog log;
 *     const OtkFrameLogConfig cfg = { otk_ns_from_ms(10), OTK_BUS_CLOCK_SIM, true, 0, "my-sim 1.0" };
 *     otk_framelog_init(&log, fp, &cfg);                 // metadata line + header
 *     otk_framelog_frame(&log, &row);                   // one row
 *
 * Two times, both on the run's timebase:
 *
 * - t:     logical time, the simulated time of the step in which the frame
 *          crossed the machine's side. Always a multiple of the step.
 *          Readers put frames on the time axis by this one.
 * - t_bus: when the frame was on the device-under-test's side, measured by
 *          the clock the file names (sim, kernel, host, bridge), or
 *          OTK_FRAMELOG_NO_TIME when not measured or never on the bus.
 *
 * Errors are sticky, as in trace.h: the first is kept and otk_framelog_ok()
 * reports it. The file belongs to the caller.
 */

#ifndef OPENTICK_FRAMELOG_H
#define OPENTICK_FRAMELOG_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <opentick/can.h>
#include <opentick/link.h>
#include <opentick/timebase.h>

/** @brief Version of the format this writer produces (the metadata line). */
#define OTK_FRAMELOG_FORMAT_VERSION 1

/** @brief A t_bus that was not measured, or a frame that never was on the bus. */
#define OTK_FRAMELOG_NO_TIME UINT64_MAX

/** @brief Which clock measured t_bus. */
typedef enum {
    OTK_BUS_CLOCK_NONE = 0, /**< not measured: t_bus is always empty */
    OTK_BUS_CLOCK_SIM,      /**< the link's simulated times (in memory): exact */
    OTK_BUS_CLOCK_KERNEL,   /**< the kernel's timestamps (SocketCAN), mapped by an anchor at t = 0 */
    OTK_BUS_CLOCK_HOST,     /**< the host's clock when the frame was read or written */
    OTK_BUS_CLOCK_BRIDGE    /**< a hard bridge's own oscillator, which is the run's clock (v0.4) */
} OtkBusClock;

/** @brief Direction, seen from the device under test. */
typedef enum {
    OTK_FRAME_TO_DUT = 0, /**< machine -> device under test */
    OTK_FRAME_FROM_DUT    /**< device under test -> machine */
} OtkFrameDir;

/** @brief What a link did to the frame (the `fault` column). */
typedef enum {
    OTK_FRAME_OK = 0,      /**< delivered unchanged (perhaps delayed) */
    OTK_FRAME_DROP,        /**< lost to drop */
    OTK_FRAME_DROP_ONE_IN, /**< lost to drop_one_in */
    OTK_FRAME_BUS_OFF,     /**< lost to bus_off, when sent or when due */
    OTK_FRAME_OVERFLOW,    /**< lost because the link's line was full */
    OTK_FRAME_CORRUPT      /**< delivered with one byte flipped */
} OtkFrameFault;

typedef struct {
    OtkNs step;           /**< step period, > 0; every t must be a multiple */
    OtkBusClock bus_clock;
    bool err_known;       /**< the bound below is stated */
    uint64_t err_ns;      /**< bound on |t_bus error|, when err_known */
    const char *source;   /**< program and version, or NULL; no ';' or line break */
} OtkFrameLogConfig;

typedef struct {
    OtkNs t;              /**< logical time */
    OtkNs t_bus;          /**< bus time, or OTK_FRAMELOG_NO_TIME */
    OtkFrameDir dir;
    const char *link;     /**< the link's name, or NULL / "" when no link carries the frame */
    const OtkCanFrame *frame; /**< as the receiver got it; as sent if it was lost */
    OtkFrameFault fault;
} OtkFrameRow;

typedef struct {
    FILE *fp;
    OtkNs step;
    OtkBusClock bus_clock;
    uint64_t rows;
    bool error;
    char error_msg[128];
} OtkFrameLog;

/**
 * @brief Writes the metadata line and the header.
 * @return false if the configuration is invalid (step 0, unknown clock, a
 *         source with ';' or a line break) or the file cannot be written
 */
bool otk_framelog_init(OtkFrameLog *log, FILE *fp, const OtkFrameLogConfig *cfg);

/**
 * @brief Writes one row. An invalid frame, a t that is not a multiple of the
 *        step, a t_bus with bus clock NONE, a lost frame with a t_bus on the
 *        DUT's side of a to_dut frame, or a link name with a comma, quote,
 *        space or line break is an error, and nothing is written.
 * @return otk_framelog_ok()
 */
bool otk_framelog_frame(OtkFrameLog *log, const OtkFrameRow *row);

/** @brief False after the first error. */
bool otk_framelog_ok(const OtkFrameLog *log);

/** @brief The first error, or "" if none. */
const char *otk_framelog_error(const OtkFrameLog *log);

/** @brief "none", "sim", "kernel", "host", "bridge"; NULL for an unknown value. */
const char *otk_bus_clock_name(OtkBusClock clock);

/** @brief "", "drop", "drop_one_in", "bus_off", "overflow", "corrupt"; NULL for an unknown value. */
const char *otk_frame_fault_name(OtkFrameFault fault);

/** @brief The `fault` of a frame from what the link reported (link.h, OtkLinkInfo). */
OtkFrameFault otk_frame_fault_from_link(const OtkLinkInfo *info);

#endif /* OPENTICK_FRAMELOG_H */
