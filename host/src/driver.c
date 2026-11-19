/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file driver.c
 * @brief Free-run and paced drivers. See driver.h.
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

void otk_driver_wait(OtkDriver *d) {
    if (d->kind == OTK_DRIVER_PACED) {
        otk_pacer_wait(&d->pacer);
    }
    d->steps++;
}

const char *otk_driver_name(OtkDriverKind kind) {
    switch (kind) {
    case OTK_DRIVER_FREE_RUN:
        return "free-run";
    case OTK_DRIVER_PACED:
        return "paced";
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
    return false;
}
