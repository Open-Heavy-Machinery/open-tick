/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file driver.c
 * @brief Free-run, paced and hard drivers. See driver.h.
 */

#include <opentick/host/driver.h>

#include <string.h>

void otk_driver_init(OtkDriver *d, OtkDriverKind kind, OtkNs step) {
    memset(d, 0, sizeof(*d));
    d->kind = kind;
    d->step = step;
    if (kind == OTK_DRIVER_PACED) {
        otk_pacer_init(&d->pacer, step);
    }
}

void otk_driver_attach_hard(OtkDriver *d, OtkHardLoop *hl) {
    d->hard = hl;
}

void otk_driver_wait(OtkDriver *d) {
    if (d->kind == OTK_DRIVER_PACED) {
        otk_pacer_wait(&d->pacer);
    } else if (d->kind == OTK_DRIVER_HARD) {
        if (d->hard == NULL || !otk_hard_loop_next(d->hard)) {
            d->failed = true;
        }
    }
    d->steps++;
}

bool otk_driver_failed(const OtkDriver *d) {
    return d->failed;
}

const char *otk_driver_name(OtkDriverKind kind) {
    switch (kind) {
    case OTK_DRIVER_FREE_RUN:
        return "free-run";
    case OTK_DRIVER_PACED:
        return "paced";
    case OTK_DRIVER_HARD:
        return "hard";
    default:
        return "?";
    }
}

bool otk_driver_from_name(const char *name, OtkDriverKind *kind) {
    if (strcmp(name, "free-run") == 0) {
        *kind = OTK_DRIVER_FREE_RUN;
        return true;
    }
    if (strcmp(name, "paced") == 0) {
        *kind = OTK_DRIVER_PACED;
        return true;
    }
    if (strcmp(name, "hard") == 0) {
        *kind = OTK_DRIVER_HARD;
        return true;
    }
    return false;
}
