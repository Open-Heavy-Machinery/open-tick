/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file hard.c
 * @brief The hard driver's link, clock mapping and cycle estimate (opentick/host/hard.h).
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE /* the B* rates above 230400 */

#include <opentick/host/hard.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------- time */

uint64_t otk_hard_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

void otk_hard_sleep_until(uint64_t t_ns) {
    struct timespec ts = { (time_t)(t_ns / 1000000000u), (long)(t_ns % 1000000000u) };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {
    }
}

/* ---------------------------------------------------------------- the link */

static speed_t rate(uint32_t baud) {
    switch (baud) {
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    case 1000000: return B1000000;
    case 1500000: return B1500000;
    case 2000000: return B2000000;
    case 2500000: return B2500000;
    case 3000000: return B3000000;
    case 4000000: return B4000000;
    default: return 0;
    }
}

int otk_hard_open(OtkHardLink *l, const char *path, uint32_t baud) {
    memset(l, 0, sizeof(*l));
    l->fd = -1;
    otk_bridge_parser_init(&l->parser);
    const speed_t sp = rate(baud);
    if (sp == 0) {
        errno = EINVAL;
        return -1;
    }
    const int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }
    struct termios t;
    if (tcgetattr(fd, &t) != 0) {
        close(fd);
        return -1;
    }
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~(tcflag_t)(CSTOPB | CRTSCTS | PARENB);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (cfsetispeed(&t, sp) != 0 || cfsetospeed(&t, sp) != 0 || tcsetattr(fd, TCSANOW, &t) != 0) {
        close(fd);
        return -1;
    }
    (void)tcflush(fd, TCIOFLUSH);
    l->fd = fd;
    return 0;
}

void otk_hard_close(OtkHardLink *l) {
    if (l->fd >= 0) {
        close(l->fd);
    }
    l->fd = -1;
}

size_t otk_hard_drain(OtkHardLink *l, uint64_t silence_ns, uint64_t max_ns) {
    size_t n = 0;
    const uint64_t start = otk_hard_now_ns();
    uint64_t last = start;
    uint8_t buf[256];
    for (;;) {
        const uint64_t now = otk_hard_now_ns();
        if (now - last >= silence_ns || now - start >= max_ns) {
            break;
        }
        struct pollfd p = { l->fd, POLLIN, 0 };
        if (poll(&p, 1, 10) > 0) {
            const ssize_t r = read(l->fd, buf, sizeof(buf));
            if (r > 0) {
                n += (size_t)r;
                last = otk_hard_now_ns();
            }
        }
    }
    otk_bridge_parser_init(&l->parser);
    l->npend = 0;
    return n;
}

static int write_all(int fd, const uint8_t *p, size_t n) {
    while (n > 0) {
        const ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                struct pollfd q = { fd, POLLOUT, 0 };
                (void)poll(&q, 1, 10);
                continue;
            }
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int otk_hard_transact(OtkHardLink *l, uint8_t type, const uint8_t *payload, size_t len, bool retransmit,
                      uint64_t timeout_ns, OtkHardReply *r) {
    if (!retransmit) {
        l->seq++;
    }
    uint8_t frame[OTK_BRIDGE_FRAMING_BYTES + OTK_BRIDGE_MAX_PAYLOAD];
    const size_t n = otk_bridge_frame(type, l->seq, payload, len, frame, sizeof(frame));
    if (n == 0) {
        errno = EINVAL;
        return -1;
    }
    r->t1 = otk_hard_now_ns();
    if (write_all(l->fd, frame, n) != 0) {
        return -1;
    }
    const uint64_t deadline = r->t1 + timeout_ns;
    OtkBridgeMessage m;
    for (;;) {
        /* Parse what is already here: held by the parser, or read and pending. */
        size_t used = 0;
        bool got = otk_bridge_parse(&l->parser, l->pend, l->npend, &used, &m);
        memmove(l->pend, &l->pend[used], l->npend - used);
        l->npend -= used;
        if (got) {
            if (m.seq == l->seq && (m.type & OTK_BRIDGE_REPLY)) {
                r->t4 = l->t_pend;
                r->type = m.type;
                r->seq = m.seq;
                r->len = m.len;
                memcpy(r->payload, m.payload, m.len);
                return 0;
            }
            continue; /* a stale reply: skip it */
        }
        const uint64_t now = otk_hard_now_ns();
        if (now >= deadline) {
            l->timeouts++;
            return 1;
        }
        const uint64_t left_ms = (deadline - now) / 1000000u + 1u;
        struct pollfd p = { l->fd, POLLIN, 0 };
        const int pr = poll(&p, 1, (int)(left_ms > 1000u ? 1000u : left_ms));
        if (pr < 0 && errno != EINTR) {
            return -1;
        }
        if (pr <= 0) {
            continue;
        }
        const ssize_t n_read = read(l->fd, &l->pend[l->npend], sizeof(l->pend) - l->npend);
        if (n_read < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            return -1;
        }
        l->npend += (size_t)n_read;
        l->t_pend = otk_hard_now_ns();
    }
}

/* ---------------------------------------------------------------- the clock mapping */

void otk_hard_sync_init(OtkHardSync *s, uint32_t tick_hz, uint64_t bin_ns) {
    memset(s, 0, sizeof(*s));
    s->tick_hz = tick_hz;
    s->bin_ns = bin_ns;
}

static double ticks_to_ns(const OtkHardSync *s, double ticks) {
    return ticks * (1e9 / (double)s->tick_hz);
}

static void refit(OtkHardSync *s) {
    uint64_t newest = 0;
    for (unsigned i = 0; i < OTK_HARD_SYNC_BINS; i++) {
        if (s->best[i].valid && s->best[i].bin > newest) {
            newest = s->best[i].bin;
        }
    }
    /* A bin whose best sample is far slower than the best of all has no good
     * sample (the link missed its forwarding instant): it is left out. */
    double lowest = INFINITY;
    for (unsigned i = 0; i < OTK_HARD_SYNC_BINS; i++) {
        if (s->best[i].valid && s->best[i].bin + OTK_HARD_SYNC_BINS > newest && s->best[i].delay < lowest) {
            lowest = s->best[i].delay;
        }
    }
    const double keep_below = lowest + OTK_HARD_SYNC_SLACK_NS;
    double st = 0, so = 0, stt = 0, sto = 0;
    unsigned n = 0;
    for (unsigned i = 0; i < OTK_HARD_SYNC_BINS; i++) {
        if (s->best[i].valid && s->best[i].bin + OTK_HARD_SYNC_BINS > newest && s->best[i].delay <= keep_below) {
            st += s->best[i].t;
            so += s->best[i].offset;
            n++;
        }
    }
    if (n == 0) {
        return;
    }
    const double mt = st / n, mo = so / n;
    for (unsigned i = 0; i < OTK_HARD_SYNC_BINS; i++) {
        if (s->best[i].valid && s->best[i].bin + OTK_HARD_SYNC_BINS > newest && s->best[i].delay <= keep_below) {
            stt += (s->best[i].t - mt) * (s->best[i].t - mt);
            sto += (s->best[i].t - mt) * (s->best[i].offset - mo);
        }
    }
    s->b = (n >= 2 && stt > 0) ? sto / stt : 0.0;
    s->a = mo - s->b * mt;
    s->residual_max = 0;
    s->delay_min = INFINITY;
    for (unsigned i = 0; i < OTK_HARD_SYNC_BINS; i++) {
        if (s->best[i].valid && s->best[i].bin + OTK_HARD_SYNC_BINS > newest && s->best[i].delay <= keep_below) {
            const double res = fabs(s->best[i].offset - (s->a + s->b * s->best[i].t));
            if (res > s->residual_max) {
                s->residual_max = res;
            }
            if (s->best[i].delay < s->delay_min) {
                s->delay_min = s->best[i].delay;
            }
        }
    }
    s->used = n;
    s->fitted = true;
}

void otk_hard_sync_add(OtkHardSync *s, const OtkHardSample *x) {
    if (!s->have_ref) {
        s->t_ref = x->t1;
        s->have_ref = true;
    }
    const double t2 = ticks_to_ns(s, (double)x->t2), t3 = ticks_to_ns(s, (double)x->t3);
    const double t1 = (double)(x->t1 - s->t_ref), t4 = (double)(x->t4 - s->t_ref);
    const double delay = (t4 - t1) - (t3 - t2);
    const double offset = ((t2 - t1) + (t3 - t4)) / 2.0; /* bridge minus (host - t_ref) */
    const double t = (t1 + t4) / 2.0;
    const uint64_t bin = (x->t1 - s->t_ref) / s->bin_ns;
    const unsigned slot = (unsigned)(bin % OTK_HARD_SYNC_BINS);
    if (!s->best[slot].valid || s->best[slot].bin != bin || delay < s->best[slot].delay) {
        s->best[slot].valid = true;
        s->best[slot].bin = bin;
        s->best[slot].t = t;
        s->best[slot].offset = offset;
        s->best[slot].delay = delay;
        refit(s);
    }
}

double otk_hard_sync_bound_ns(const OtkHardSync *s) {
    return s->fitted ? s->residual_max + s->delay_min / 2.0 : INFINITY;
}

double otk_hard_sync_to_bridge(const OtkHardSync *s, uint64_t t_ns) {
    const double u = (double)(int64_t)(t_ns - s->t_ref);
    const double bridge_ns = u + s->a + s->b * u;
    return bridge_ns * ((double)s->tick_hz / 1e9);
}

uint64_t otk_hard_sync_to_host(const OtkHardSync *s, double tick) {
    const double bridge_ns = ticks_to_ns(s, tick);
    const double u = (bridge_ns - s->a) / (1.0 + s->b);
    return s->t_ref + (uint64_t)(int64_t)llround(u);
}

/* ---------------------------------------------------------------- the forwarding cycle */

static int cmp_double(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

bool otk_hard_cycle_fit(const uint64_t *t, size_t n, double p_guess, double *period, double *t0, double *rms) {
    if (n < 8) {
        return false;
    }
    double *gaps = malloc(n * sizeof(double));
    double *k = malloc(n * sizeof(double));
    if (!gaps || !k) {
        free(gaps);
        free(k);
        return false;
    }
    size_t ng = 0;
    /* every gap near a whole number of cycles gives one estimate of the period */
    for (size_t i = 1; i < n; i++) {
        const double g = (double)(t[i] - t[i - 1]);
        const double cycles = nearbyint(g / p_guess);
        if (cycles >= 1.0 && fabs(g / p_guess - cycles) < 0.25) {
            gaps[ng++] = g / cycles;
        }
    }
    bool ok = false;
    if (ng >= 7) {
        qsort(gaps, ng, sizeof(double), cmp_double);
        const double p1 = gaps[ng / 2];
        /* cycle numbers, gap by gap, so that a slightly wrong p1 cannot accumulate */
        k[0] = 0;
        for (size_t i = 1; i < n; i++) {
            k[i] = k[i - 1] + nearbyint((double)(t[i] - t[i - 1]) / p1);
        }
        double sk = 0, st = 0, skk = 0, skt = 0;
        for (size_t i = 0; i < n; i++) {
            const double ti = (double)(t[i] - t[0]);
            sk += k[i];
            st += ti;
        }
        const double mk = sk / (double)n, mt = st / (double)n;
        for (size_t i = 0; i < n; i++) {
            const double ti = (double)(t[i] - t[0]);
            skk += (k[i] - mk) * (k[i] - mk);
            skt += (k[i] - mk) * (ti - mt);
        }
        if (skk > 0) {
            *period = skt / skk;
            *t0 = (double)t[0] + mt - *period * mk;
            double ss = 0;
            for (size_t i = 0; i < n; i++) {
                const double e = (double)t[i] - (*t0 + *period * k[i]);
                ss += e * e;
            }
            *rms = sqrt(ss / (double)n);
            ok = true;
        }
    }
    free(gaps);
    free(k);
    return ok;
}
