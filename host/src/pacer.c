/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file pacer.c
 * @brief Wall-clock pacing. See pacer.h.
 */

#define _POSIX_C_SOURCE 200809L

#include <opentick/host/pacer.h>

#include <errno.h>

static OtkNs to_ns(const struct timespec *ts) {
    return (OtkNs)ts->tv_sec * OTK_NS_PER_S + (OtkNs)ts->tv_nsec;
}

static void now(struct timespec *ts) {
    (void)clock_gettime(CLOCK_MONOTONIC, ts);
}

void otk_pacer_init(OtkPacer *p, OtkNs period) {
    p->period = (period > 0U) ? period : 1U;
    p->overruns = 0U;
    p->last_late = 0U;
    p->max_late = 0U;
    now(&p->next);
}

static void record_late(OtkPacer *p, OtkNs late) {
    p->last_late = late;
    if (late > p->max_late) {
        p->max_late = late;
    }
}

void otk_pacer_wait(OtkPacer *p) {
    p->next.tv_sec += (time_t)(p->period / OTK_NS_PER_S);
    p->next.tv_nsec += (long)(p->period % OTK_NS_PER_S);
    if (p->next.tv_nsec >= (long)OTK_NS_PER_S) {
        p->next.tv_sec += 1;
        p->next.tv_nsec -= (long)OTK_NS_PER_S;
    }

    struct timespec t;
    now(&t);
    const OtkNs deadline = to_ns(&p->next);
    if (to_ns(&t) > deadline) {
        p->overruns++;
        record_late(p, to_ns(&t) - deadline);
        p->next = t; /* do not try to catch up with a burst of iterations */
        return;
    }
    int rc;
    while ((rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &p->next, NULL)) == EINTR) {
        /* interrupted by a signal: sleep again until the deadline */
    }
    now(&t);
    record_late(p, (to_ns(&t) > deadline) ? to_ns(&t) - deadline : 0U);
}
