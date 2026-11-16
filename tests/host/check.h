/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal checks for the host unit tests: CHECK counts a failure and goes on,
 * so one run lists every broken case. check_exit() gives the exit status.
 */
#ifndef CTUCAN_HOST_CHECK_H
#define CTUCAN_HOST_CHECK_H

#include <stdio.h>

static unsigned check_run, check_failed;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        check_run++;                                                        \
        if (!(cond)) {                                                      \
            check_failed++;                                                 \
            fprintf(stderr, "%s:%d: FAIL %s: ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                                   \
            fputc('\n', stderr);                                            \
        }                                                                   \
    } while (0)

#define CHECK_EQ(a, b) \
    CHECK((unsigned long long)(a) == (unsigned long long)(b), "%llu != %llu", \
          (unsigned long long)(a), (unsigned long long)(b))

static inline int check_exit(const char *name)
{
    printf("%s: %u checks, %u failed\n", name, check_run, check_failed);
    return check_failed == 0 ? 0 : 1;
}

#endif
