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

#include <opentick/can.h>

typedef struct {
    int fd;          /**< raw CAN socket, non-blocking; -1 when closed */
    bool fd_capable; /**< the interface's MTU allows CAN FD frames */
} OtkSocketCan;

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

#endif /* OPENTICK_HOST_SOCKETCAN_H */
