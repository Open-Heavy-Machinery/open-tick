/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file socketcan.c
 * @brief Linux SocketCAN bridge. See socketcan.h.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <opentick/host/socketcan.h>

#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <linux/can.h>
#include <linux/can/raw.h>

bool otk_socketcan_open(OtkSocketCan *s, const char *ifname) {
    s->fd = -1;
    s->fd_capable = false;

    if (strlen(ifname) >= IFNAMSIZ) {
        errno = ENAMETOOLONG;
        return false;
    }
    int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) {
        return false;
    }

    /* Non-blocking, so that draining the receive queue never stalls a step. */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        goto fail;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    memcpy(ifr.ifr_name, ifname, strlen(ifname));
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        goto fail;
    }
    const int ifindex = ifr.ifr_ifindex;

    /* Receiving FD frames is harmless on a classic interface; sending them
     * is only possible when the interface's MTU allows it. */
    const int enable = 1;
    if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &enable, sizeof(enable)) < 0) {
        goto fail;
    }
    if (ioctl(fd, SIOCGIFMTU, &ifr) == 0) {
        s->fd_capable = ifr.ifr_mtu == (int)CANFD_MTU;
    }

    struct sockaddr_can addr;
    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifindex;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        goto fail;
    }
    s->fd = fd;
    return true;

fail:;
    const int saved = errno;
    close(fd);
    errno = saved;
    return false;
}

void otk_socketcan_close(OtkSocketCan *s) {
    if (s->fd >= 0) {
        close(s->fd);
    }
    s->fd = -1;
}

static canid_t wire_id(const OtkCanFrame *f) {
    if ((f->flags & OTK_CAN_EXTENDED) != 0U) {
        return (f->id & CAN_EFF_MASK) | CAN_EFF_FLAG;
    }
    return f->id & CAN_SFF_MASK;
}

bool otk_socketcan_send(OtkSocketCan *s, const OtkCanFrame *f) {
    if (!otk_can_frame_valid(f)) {
        errno = EINVAL;
        return false;
    }
    if ((f->flags & OTK_CAN_FD) != 0U) {
        if (!s->fd_capable) {
            errno = EMSGSIZE;
            return false;
        }
        struct canfd_frame frame;
        memset(&frame, 0, sizeof(frame));
        frame.can_id = wire_id(f);
        frame.len = f->len;
        frame.flags = ((f->flags & OTK_CAN_BRS) != 0U) ? CANFD_BRS : 0U;
        memcpy(frame.data, f->data, f->len);
        /* A full queue or bus-off loses the frame, as on a real bus. */
        return write(s->fd, &frame, CANFD_MTU) == (ssize_t)CANFD_MTU;
    }
    struct can_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.can_id = wire_id(f);
    frame.can_dlc = f->len;
    memcpy(frame.data, f->data, f->len);
    return write(s->fd, &frame, CAN_MTU) == (ssize_t)CAN_MTU;
}

int otk_socketcan_recv(OtkSocketCan *s, OtkCanFrame *f) {
    for (;;) {
        struct canfd_frame frame;
        const ssize_t n = read(s->fd, &frame, sizeof(frame));
        if (n < 0) {
            return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
        }
        const bool fd = n == (ssize_t)CANFD_MTU;
        if (!fd && n != (ssize_t)CAN_MTU) {
            continue; /* not a frame we know */
        }
        if ((frame.can_id & (CAN_RTR_FLAG | CAN_ERR_FLAG)) != 0U) {
            continue;
        }
        memset(f, 0, sizeof(*f));
        if ((frame.can_id & CAN_EFF_FLAG) != 0U) {
            f->flags = OTK_CAN_EXTENDED;
            f->id = frame.can_id & CAN_EFF_MASK;
        } else {
            f->id = frame.can_id & CAN_SFF_MASK;
        }
        /* struct can_frame and struct canfd_frame share their layout up to
         * the data, so the classic length is in frame.len too. */
        uint8_t len = frame.len;
        if (fd) {
            f->flags |= OTK_CAN_FD;
            if ((frame.flags & CANFD_BRS) != 0U) {
                f->flags |= OTK_CAN_BRS;
            }
            if (len > CANFD_MAX_DLEN) {
                len = CANFD_MAX_DLEN;
            }
        } else if (len > CAN_MAX_DLEN) {
            len = CAN_MAX_DLEN;
        }
        f->len = len;
        memcpy(f->data, frame.data, len);
        return 1;
    }
}
