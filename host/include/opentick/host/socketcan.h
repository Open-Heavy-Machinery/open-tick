/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file socketcan.h
 * @brief Host bridge for CAN: Linux SocketCAN, classic CAN and CAN FD.
 *
 * Works the same on a virtual bus (`vcan`) and on a USB-CAN adapter that
 * Linux supports. Frames go out and come in as they are; what they mean is
 * the application's business. Remote (RTR) and error frames are skipped on
 * receive; 11-bit and 29-bit frames are both delivered, with OTK_CAN_EXTENDED
 * telling them apart.
 *
 * CAN FD frames need an interface whose MTU allows them (`ip link set vcan0
 * mtu 72` for vcan). Sending one on a classic interface fails instead of
 * being truncated.
 *
 * Extracted from open-crank v0.1 (platforms/linux-socketcan/socketcan.c),
 * which handled 29-bit classic frames only.
 */

#ifndef OPENTICK_HOST_SOCKETCAN_H
#define OPENTICK_HOST_SOCKETCAN_H

#include <stdbool.h>
#include <stdint.h>

#include <opentick/can.h>
#include <opentick/framelog.h>

typedef struct {
    int fd;          /**< raw CAN socket, non-blocking; -1 when closed */
    bool fd_capable; /**< the interface's MTU allows CAN FD frames */
    bool timestamps; /**< otk_socketcan_enable_timestamps() succeeded */
    bool own_msgs;   /**< this socket's own frames come back to it */
} OtkSocketCan;

/** @brief What otk_socketcan_recv_ex() knows about a received frame. */
typedef struct {
    bool own;         /**< a frame this socket sent, back from the kernel (MSG_CONFIRM) */
    bool has_time;    /**< the kernel stamped it */
    uint64_t time_ns; /**< the kernel's timestamp, on otk_socketcan_clock_ns()'s clock */
} OtkSocketCanRxInfo;

/**
 * @brief Opens a non-blocking raw CAN socket bound to @p ifname.
 * @return false on error, with errno set
 */
bool otk_socketcan_open(OtkSocketCan *s, const char *ifname);

/** @brief Closes the socket. Safe to call twice. */
void otk_socketcan_close(OtkSocketCan *s);

/**
 * @brief Sends one frame.
 * @return false if the frame is invalid (errno EINVAL), is CAN FD on a classic
 *         interface (errno EMSGSIZE), or was not queued (full queue, bus-off:
 *         the frame is lost, as on a real bus)
 */
bool otk_socketcan_send(OtkSocketCan *s, const OtkCanFrame *f);

/**
 * @brief Receives one frame without blocking. Payload bytes past the frame's
 *        length are zero.
 * @return 1 if @p f was filled, 0 if nothing is waiting, -1 on error
 */
int otk_socketcan_recv(OtkSocketCan *s, OtkCanFrame *f);

/* ---- Timestamps, for a frame log (framelog.h) ------------------------------ */

/**
 * @brief Asks the kernel to stamp every received frame (SO_TIMESTAMPNS) and,
 *        if @p own_msgs, to give this socket back each frame it sent
 *        (CAN_RAW_RECV_OWN_MSGS), stamped when it left on the interface.
 *        otk_socketcan_recv() then skips the socket's own frames, so code
 *        that does not look at them sees what it saw before.
 * @return false on error, with errno set; the socket is unchanged
 */
bool otk_socketcan_enable_timestamps(OtkSocketCan *s, bool own_msgs);

/**
 * @brief otk_socketcan_recv(), with what the kernel says about the frame in
 *        @p info: its timestamp, and whether it is one of this socket's own.
 *        Own frames are returned (not skipped) here.
 * @return 1 if @p f was filled, 0 if nothing is waiting, -1 on error
 */
int otk_socketcan_recv_ex(OtkSocketCan *s, OtkCanFrame *f, OtkSocketCanRxInfo *info);

/**
 * @brief Now, on the clock of the kernel's timestamps (CLOCK_REALTIME), in
 *        nanoseconds. Take it once at t = 0 as the anchor.
 */
uint64_t otk_socketcan_clock_ns(void);

/**
 * @brief A kernel timestamp on the run's timebase: @p time_ns - @p anchor_ns,
 *        or OTK_FRAMELOG_NO_TIME if it is before the anchor.
 */
OtkNs otk_socketcan_run_time(uint64_t time_ns, uint64_t anchor_ns);

/**
 * @brief The bus clock a frame log over this socket states: none if the run
 *        is not paced (wall time and simulated time are unrelated), kernel if
 *        timestamps are on, otherwise host.
 */
OtkBusClock otk_socketcan_bus_clock(const OtkSocketCan *s, bool paced);

#endif /* OPENTICK_HOST_SOCKETCAN_H */
