/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file fake_bridge.c
 * @brief A bridge on a pseudo-terminal for the hard driver's tests (fake_bridge.h).
 */

#define _GNU_SOURCE /* openpty */

#include "fake_bridge.h"

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <pty.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

void fake_init(FakeBridge *f) {
    memset(f, 0, sizeof(*f));
    f->cycle_ns = 2000000.0;
    f->initial_state = OTK_BRIDGE_IDLE;
    f->n_analog = 4u;
    f->n_capture = 6u;
    f->drop_setpoint_at = UINT32_MAX;
    f->report_late_at = UINT32_MAX;
    f->silent_from = UINT32_MAX;
    f->capture_at = UINT32_MAX;
    f->wrong_applied_from = UINT32_MAX;
    f->offset_ticks = 5.0e9;
    f->fd = -1;
}

uint64_t fake_tick(const FakeBridge *f, uint64_t mono) {
    const double dt = (double)(int64_t)(mono - f->base_mono);
    return (uint64_t)llround(f->offset_ticks + dt * 0.027 * (1.0 + f->drift));
}

static void pop(FakeBridge *f) {
    memmove(&f->buf[0], &f->buf[1], (f->nbuf - 1u) * sizeof(f->buf[0]));
    memmove(&f->buf_step[0], &f->buf_step[1], (f->nbuf - 1u) * sizeof(f->buf_step[0]));
    f->nbuf--;
}

static void rest(FakeBridge *f) {
    memset(&f->applied_now, 0, sizeof(f->applied_now));
    f->nbuf = 0;
}

/* Begins every step whose time has come, up to `tick`. */
static void advance(FakeBridge *f, uint64_t tick) {
    while (f->state == OTK_BRIDGE_RUNNING) {
        const uint64_t begin = f->t_start + (uint64_t)f->next_step * f->step_ticks;
        if (begin > tick) {
            break;
        }
        if (f->link_timeout_steps != 0u &&
            begin > f->last_rx_tick + (uint64_t)f->link_timeout_steps * f->step_ticks) {
            f->state = OTK_BRIDGE_ABORTED; /* §3.5 */
            rest(f);
            break;
        }
        const uint32_t k = f->next_step;
        while (f->nbuf > 0u && f->buf_step[0] < k) {
            pop(f);
            f->pending_err |= OTK_BRIDGE_ERR_LATE;
            f->pending_count++;
        }
        if (f->nbuf > 0u && f->buf_step[0] == k) {
            f->applied_now = f->buf[0];
            if (k >= f->wrong_applied_from) {
                f->applied_now.analog[3] ^= 1u;
            }
            pop(f);
        } else if (f->underruns != UINT32_MAX) {
            f->underruns++; /* hold the last setpoint */
        }
        if (k < FAKE_MAX_STEPS) {
            f->applied[k] = f->applied_now;
            f->applied_set[k] = true;
        }
        if (k == f->capture_at && !f->capture_done && f->ncaps < 8u) {
            f->caps[f->ncaps++] = (OtkBridgeCapture){ 2u, (uint32_t)((begin + 1000u) & 0xFFFFFFFFu), 5000u };
            f->capture_done = true;
        }
        f->cur_step = k;
        f->begun = true;
        f->next_step++;
    }
}

/* Builds the reply to one message; returns its frame length, or 0 for no reply. */
static size_t handle(FakeBridge *f, const OtkBridgeMessage *m, uint64_t t2, uint8_t *out, size_t cap) {
    advance(f, t2);
    if (f->state == OTK_BRIDGE_RUNNING && f->begun && f->cur_step >= f->silent_from) {
        return 0;
    }
    f->requests++;
    f->last_rx_tick = t2;
    uint8_t pay[OTK_BRIDGE_MAX_PAYLOAD];
    size_t len = 0;
    uint8_t type = (uint8_t)(m->type | OTK_BRIDGE_REPLY);
    uint16_t err = 0;
    switch (m->type) {
    case OTK_BRIDGE_CONFIGURE: {
        OtkBridgeConfig c;
        OtkBridgeConfigReply r = { OTK_BRIDGE_STATUS_OK, 27000000u, f->n_capture, f->n_analog, 16u };
        if (!otk_bridge_decode_configure(m->payload, m->len, &c) || f->state == OTK_BRIDGE_RUNNING) {
            r.status = OTK_BRIDGE_STATUS_WRONG_STATE;
        } else {
            f->state = OTK_BRIDGE_CONFIGURED;
            f->step_ticks = c.step_ticks;
            f->link_timeout_steps = c.link_timeout_steps;
            rest(f);
            f->underruns = 0;
            f->last_queued = 0;
            f->have_seq = false;
        }
        len = otk_bridge_encode_configure_reply(&r, pay, sizeof(pay));
        break;
    }
    case OTK_BRIDGE_START: {
        OtkBridgeStartReply r = { OTK_BRIDGE_STATUS_WRONG_STATE, 0u };
        if (f->state == OTK_BRIDGE_CONFIGURED && f->nbuf > 0u && f->buf_step[0] == 0u) {
            f->state = OTK_BRIDGE_RUNNING;
            f->t_start = t2 + f->step_ticks;
            f->next_step = 0;
            f->begun = false;
            r.status = OTK_BRIDGE_STATUS_OK;
            r.t_start = f->t_start;
        }
        len = otk_bridge_encode_start_reply(&r, pay, sizeof(pay));
        break;
    }
    case OTK_BRIDGE_STOP: {
        OtkBridgeStopReply r = { OTK_BRIDGE_STATUS_WRONG_STATE, f->cur_step, f->underruns };
        if (f->state == OTK_BRIDGE_RUNNING) {
            f->state = OTK_BRIDGE_STOPPED;
            f->stop_step = f->cur_step;
            f->stopped = true;
            rest(f);
            r.status = OTK_BRIDGE_STATUS_OK;
        }
        len = otk_bridge_encode_stop_reply(&r, pay, sizeof(pay));
        break;
    }
    case OTK_BRIDGE_EXCHANGE: {
        OtkBridgeRequest q;
        if (!otk_bridge_decode_request(m->payload, m->len, &q) || f->state == OTK_BRIDGE_IDLE ||
            f->state == OTK_BRIDGE_STOPPED) {
            err |= OTK_BRIDGE_ERR_STATE;
        } else if (f->state == OTK_BRIDGE_ABORTED) {
            err |= OTK_BRIDGE_ERR_ABORTED;
        } else if (f->have_seq && m->seq == f->last_seq) {
            /* a retransmission: answered again, nothing applied */
        } else if (q.n > 0u && q.first_step != f->last_queued) {
            err |= OTK_BRIDGE_ERR_SEQUENCE;
        } else {
            for (unsigned i = 0; i < q.n; i++) {
                const uint32_t s = q.first_step + i;
                f->last_queued = s + 1u;
                if (s == f->drop_setpoint_at) {
                    continue;
                }
                if (f->state == OTK_BRIDGE_RUNNING && s < f->next_step) {
                    f->pending_err |= OTK_BRIDGE_ERR_LATE;
                    f->pending_count++;
                    continue;
                }
                if (f->nbuf < FAKE_BUF) {
                    f->buf[f->nbuf] = q.sp[i];
                    f->buf_step[f->nbuf] = s;
                    f->nbuf++;
                }
            }
        }
        if (f->state == OTK_BRIDGE_RUNNING && f->begun && f->cur_step >= f->report_late_at && !f->late_reported) {
            f->pending_err |= OTK_BRIDGE_ERR_LATE;
            f->pending_count++;
            f->late_reported = true;
        }
        OtkBridgeReply r;
        memset(&r, 0, sizeof(r));
        r.t2 = t2;
        r.t3 = t2 + 700u; /* 26 us inside the bridge */
        r.step = f->cur_step;
        r.state = f->state;
        r.fill = (uint16_t)f->nbuf;
        r.underruns = f->underruns;
        r.error = (uint16_t)(err | f->pending_err | (f->state == OTK_BRIDGE_ABORTED ? OTK_BRIDGE_ERR_ABORTED : 0u));
        r.error_count = f->pending_count;
        r.applied = f->applied_now;
        r.n_cap = (uint8_t)f->ncaps;
        memcpy(r.cap, f->caps, f->ncaps * sizeof(OtkBridgeCapture));
        f->ncaps = 0;
        f->pending_err = 0;
        f->pending_count = 0;
        len = otk_bridge_encode_reply(&r, pay, sizeof(pay));
        break;
    }
    default:
        type = OTK_BRIDGE_EXCHANGE | OTK_BRIDGE_REPLY;
        len = 0;
        break;
    }
    f->last_seq = m->seq;
    f->have_seq = true;
    return otk_bridge_frame(type, m->seq, pay, len, out, cap);
}

static void wait_instant(const FakeBridge *f) {
    if (f->cycle_ns <= 0.0) {
        return;
    }
    const uint64_t now = mono_ns();
    const double k = ceil((double)(now - f->base_mono) / f->cycle_ns);
    const uint64_t at = f->base_mono + (uint64_t)llround(k * f->cycle_ns);
    struct timespec ts = { (time_t)(at / 1000000000u), (long)(at % 1000000000u) };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {
    }
}

static void *run(void *arg) {
    FakeBridge *f = arg;
    OtkBridgeParser p;
    otk_bridge_parser_init(&p);
    const char *banner = "BL616 fake console\r\n";
    (void)!write(f->fd, banner, strlen(banner));
    uint8_t in[512], out[OTK_BRIDGE_FRAMING_BYTES + OTK_BRIDGE_MAX_PAYLOAD];
    while (!atomic_load(&f->quit)) {
        struct pollfd q = { f->fd, POLLIN, 0 };
        if (poll(&q, 1, 10) <= 0) {
            continue;
        }
        const ssize_t n = read(f->fd, in, sizeof(in));
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
                continue;
            }
            usleep(1000); /* the host side closed: wait to be told to quit */
            continue;
        }
        const uint64_t t2 = fake_tick(f, mono_ns());
        size_t at = 0;
        while (at < (size_t)n) {
            size_t used = 0;
            OtkBridgeMessage m;
            const bool got = otk_bridge_parse(&p, &in[at], (size_t)n - at, &used, &m);
            at += used;
            if (!got) {
                continue;
            }
            const size_t fl = handle(f, &m, t2, out, sizeof(out));
            if (fl > 0u) {
                wait_instant(f);
                (void)!write(f->fd, out, fl);
            }
        }
    }
    return NULL;
}

const char *fake_start(FakeBridge *f, char *name, size_t cap) {
    int master = -1, slave = -1;
    char path[256];
    if (openpty(&master, &slave, path, NULL, NULL) != 0) {
        return NULL;
    }
    struct termios t;
    tcgetattr(master, &t);
    cfmakeraw(&t);
    tcsetattr(master, TCSANOW, &t);
    close(slave);
    f->fd = master;
    f->state = f->initial_state;
    f->base_mono = mono_ns();
    atomic_init(&f->quit, false);
    snprintf(name, cap, "%s", path);
    if (pthread_create(&f->th, NULL, run, f) != 0) {
        close(master);
        return NULL;
    }
    return name;
}

void fake_stop(FakeBridge *f) {
    atomic_store(&f->quit, true);
    pthread_join(f->th, NULL);
    close(f->fd);
    f->fd = -1;
}
