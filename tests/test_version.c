/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_version.c
 * @brief version.h agrees with the version CMake was given.
 */

#include <string.h>

#include "check.h"
#include <opentick/version.h>

#define STR2(x) #x
#define STR(x) STR2(x)

int main(void) {
    CHECK(strcmp(OTK_VERSION_STRING, OTK_PROJECT_VERSION) == 0);
    CHECK(strcmp(OTK_VERSION_PRERELEASE, OTK_PROJECT_PRERELEASE) == 0);
    {
        /* MAJOR.MINOR.PATCH, then -PRERELEASE when there is one */
        const char *numbers = STR(OTK_VERSION_MAJOR) "." STR(OTK_VERSION_MINOR) "." STR(OTK_VERSION_PATCH);
        const size_t n = strlen(numbers);
        CHECK(strncmp(OTK_VERSION_STRING, numbers, n) == 0);
        if (OTK_VERSION_PRERELEASE[0] == '\0') {
            CHECK(OTK_VERSION_STRING[n] == '\0');
        } else {
            CHECK(OTK_VERSION_STRING[n] == '-' && strcmp(&OTK_VERSION_STRING[n + 1], OTK_VERSION_PRERELEASE) == 0);
        }
    }
    return check_report();
}
