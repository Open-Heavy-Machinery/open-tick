/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file fake_bridge_main.c
 * @brief otk-fake-bridge: the tests' fake bridge (fake_bridge.h) as a
 *        program, so that a whole example can run its hard driver without a
 *        board. It opens a pseudo-terminal, links its path to @p link, serves
 *        one host on it with a 6 ms forwarding cycle (the BL616's), and exits
 *        after @p seconds or when its link file is removed.
 *
 *     otk-fake-bridge <link> <seconds> [silent_from_step]
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "fake_bridge.h"

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: otk-fake-bridge <link> <seconds> [silent_from_step]\n");
        return 2;
    }
    static FakeBridge f;
    fake_init(&f);
    f.cycle_ns = 5999300.0;
    f.drift = -6e-6;
    if (argc > 3) {
        f.silent_from = (uint32_t)strtoul(argv[3], NULL, 10);
    }
    char name[256];
    if (fake_start(&f, name, sizeof(name)) == NULL) {
        perror("otk-fake-bridge: no pseudo-terminal");
        return 1;
    }
    (void)unlink(argv[1]);
    if (symlink(name, argv[1]) != 0) {
        perror("otk-fake-bridge: cannot link the pty");
        fake_stop(&f);
        return 1;
    }
    const long seconds = strtol(argv[2], NULL, 10);
    for (long i = 0; i < seconds * 10 && access(argv[1], F_OK) == 0; i++) {
        usleep(100000);
    }
    fake_stop(&f);
    (void)unlink(argv[1]);
    printf("otk-fake-bridge: %u requests, %u underruns, %s\n", f.requests, f.underruns,
           f.stopped ? "stopped by the host" : "not stopped");
    return 0;
}
