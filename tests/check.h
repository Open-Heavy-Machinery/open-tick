/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file check.h
 * @brief Minimal test macros. Unlike assert(), CHECK always runs: it does not
 *        disappear under -DNDEBUG, so Release builds still test.
 */

#ifndef OTK_TEST_CHECK_H
#define OTK_TEST_CHECK_H

#include <math.h>
#include <stdio.h>

static int otk_checks = 0;
static int otk_failures = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        otk_checks++;                                                                   \
        if (!(cond)) {                                                                 \
            otk_failures++;                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                              \
    } while (0)

#define CHECK_NEAR(actual, expected, tol)                                              \
    do {                                                                               \
        const double a_ = (double)(actual);                                            \
        const double e_ = (double)(expected);                                          \
        otk_checks++;                                                                   \
        if (!(fabs(a_ - e_) <= (double)(tol))) {                                       \
            otk_failures++;                                                             \
            fprintf(stderr, "%s:%d: CHECK_NEAR failed: %s = %.6g, expected %.6g +/- %.3g\n", \
                    __FILE__, __LINE__, #actual, a_, e_, (double)(tol));               \
        }                                                                              \
    } while (0)

#define RUN_TEST(fn)                                                                   \
    do {                                                                               \
        const int before_ = otk_failures;                                               \
        fn();                                                                          \
        printf("%s  %s\n", (otk_failures == before_) ? "PASS" : "FAIL", #fn);          \
    } while (0)

/** @brief Prints the summary and yields the process exit code. */
static int check_report(void) {
    printf("%d checks, %d failed\n", otk_checks, otk_failures);
    return (otk_failures == 0) ? 0 : 1;
}

#endif /* OTK_TEST_CHECK_H */
