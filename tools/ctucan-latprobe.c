/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * ctucan-latprobe DEV MS
 *
 * Sends CAN_DEVCTL_GET_STATS on DEV back to back for MS milliseconds and
 * reports how long the driver took to answer: the longest answer, when it
 * came (ms from the start), and how many took over 1, 5 and 10 ms. A driver
 * whose request thread sleeps or waits shows it here as one long answer.
 * It uses only the standard CAN devctl, so it runs against any CAN driver,
 * this one's main branch included (scripts/tests/signature.py).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <devctl.h>
#include <sys/can_dcmd.h>

static uint64_t mono_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "usage: ctucan-latprobe DEV MS\n");
        return 2;
    }
    int fd = open(argv[1], O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "open %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    uint64_t t0 = mono_us(), end = t0 + strtoull(argv[2], NULL, 0) * 1000u;
    uint64_t worst = 0, worst_at = 0, n = 0, over1 = 0, over5 = 0, over10 = 0;
    CAN_DEVCTL_STATS st;

    while (mono_us() < end) {
        uint64_t t = mono_us();
        int rc = devctl(fd, CAN_DEVCTL_GET_STATS, &st, sizeof(st), NULL);
        uint64_t dt = mono_us() - t;
        if (rc != EOK) {
            fprintf(stderr, "CAN_DEVCTL_GET_STATS: %s\n", strerror(rc));
            return 1;
        }
        n++;
        over1 += dt > 1000;
        over5 += dt > 5000;
        over10 += dt > 10000;
        if (dt > worst) {
            worst = dt;
            worst_at = t - t0;
        }
    }
    printf("latprobe: %llu devctls, longest %llu us at %llu ms, over 1 ms %llu, over 5 ms %llu, over 10 ms %llu\n",
           (unsigned long long)n, (unsigned long long)worst, (unsigned long long)(worst_at / 1000u),
           (unsigned long long)over1, (unsigned long long)over5, (unsigned long long)over10);
    return 0;
}
