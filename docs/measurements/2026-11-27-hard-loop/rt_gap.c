/* SPDX-License-Identifier: Apache-2.0 */
/* How wide the REALTIME -> MONOTONIC samples of docs/bridge-protocol.md §5
 * are on this host: 10 000 triple reads (REALTIME, MONOTONIC, REALTIME),
 * 6 ms apart (about one per exchange), with the same offset as OtkRtMono
 * (the middle of the two REALTIME reads minus MONOTONIC). It prints the
 * widest gap between the two REALTIME reads, how far the offset moved, and
 * how the gaps spread. Results on the board host: rt_gap.txt.
 *
 *   cc -O2 -o rt_gap rt_gap.c && ./rt_gap
 */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <time.h>

static uint64_t ns(clockid_t c) {
    struct timespec t;
    clock_gettime(c, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

int main(void) {
    uint64_t g[6] = {0};
    int64_t omin = INT64_MAX, omax = INT64_MIN;
    uint64_t maxg = 0;
    int n;
    const struct timespec s = {0, 6000000};
    for (n = 0; n < 10000; n++) {
        const uint64_t a = ns(CLOCK_REALTIME), b = ns(CLOCK_MONOTONIC), c = ns(CLOCK_REALTIME);
        const uint64_t d = c - a;
        if (d > maxg) {
            maxg = d;
        }
        const int k = d < 1000 ? 0 : d < 2000 ? 1 : d < 5000 ? 2 : d < 10000 ? 3 : d < 20000 ? 4 : 5;
        g[k]++;
        const int64_t o = (int64_t)(a + (c - a) / 2 - b);
        if (o < omin) {
            omin = o;
        }
        if (o > omax) {
            omax = o;
        }
        nanosleep(&s, 0);
    }
    printf("samples=%d max_gap_ns=%llu offset_range_ns=%lld  <1us=%llu 1-2us=%llu 2-5us=%llu 5-10us=%llu "
           "10-20us=%llu >=20us=%llu\n",
           n, (unsigned long long)maxg, (long long)(omax - omin), (unsigned long long)g[0],
           (unsigned long long)g[1], (unsigned long long)g[2], (unsigned long long)g[3], (unsigned long long)g[4],
           (unsigned long long)g[5]);
    return 0;
}
