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
    CHECK(strcmp(OTK_VERSION_STRING, STR(OTK_VERSION_MAJOR) "." STR(OTK_VERSION_MINOR) "." STR(OTK_VERSION_PATCH)) == 0);
    return check_report();
}
