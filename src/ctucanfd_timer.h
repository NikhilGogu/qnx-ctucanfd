/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The one-shot wake-up of dev-can-ctucanfd's TX waits (docs/DESIGN.md,
 * section 5). libcan runs every driver callback on its one resource-manager
 * thread, which must never sleep, and it offers the driver no way to receive
 * a pulse of its own. A thread of the driver cannot open the driver's own
 * device either (ENOENT: libcan's paths lack _RESMGR_FLAG_SELF). So the
 * timer's pulse goes to a helper process, which sends CTUCAN_DEVCTL_TIMER to
 * the driver's device; libcan then runs that devctl on its thread like any
 * other. The helper never touches driver state.
 *
 * src/ctucanfd_timer.c is the QNX implementation; the host unit tests use a
 * fake (tests/host/fake_libcan.c).
 */
#ifndef CTUCANFD_TIMER_H
#define CTUCANFD_TIMER_H

#include <stdint.h>

/* Forks the helper process and creates the timer. The helper opens `path`
 * (a device of this driver) once libcan's loop answers, and sends devctl
 * `dcmd` there for each timer expiry. Call while the process has one thread
 * and holds nothing a child should not inherit: in main(), right after the
 * arguments are parsed. Sets SIGCHLD to SA_NOCLDWAIT. Returns EOK or an errno
 * value; on failure nothing is left behind. */
int ctucan_timer_init(const char *path, int dcmd);

/* Arms the timer for CLOCK_MONOTONIC time `at_ms` (milliseconds, as the
 * driver's now_ms()); a time already past fires at once. 0 disarms. Never
 * blocks. Does nothing before ctucan_timer_init() or after
 * ctucan_timer_fini(). */
void ctucan_timer_arm(uint64_t at_ms);

/* Deletes the timer and drops the connection to the helper, which then ends
 * by itself. Safe to call more than once. */
void ctucan_timer_fini(void);

#endif
