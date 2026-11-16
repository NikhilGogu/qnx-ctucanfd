/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests only. The process manager event call from QNX's
 * <sys/procmgr.h> that tools/ctucan-supervisor.c uses, written from that use,
 * not copied from the QNX header; the value is not QNX's.
 * tests/host/test_supervisor.c implements it. Never put this directory on the
 * include path of a QNX build.
 */
#ifndef CTUCAN_HOST_SHIM_PROCMGR_H
#define CTUCAN_HOST_SHIM_PROCMGR_H

#include <signal.h>

#define PROCMGR_EVENT_PROCESS_DEATH 0x0100

int procmgr_event_notify(unsigned flags, const struct sigevent *ev);

#endif
