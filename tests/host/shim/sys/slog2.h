/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests only. The three slog2 severities src/dev-can-ctucanfd.c
 * uses, written from that use, not copied from QNX's <sys/slog2.h>. Never put
 * this directory on the include path of a QNX build.
 */
#ifndef CTUCAN_HOST_SHIM_SLOG2_H
#define CTUCAN_HOST_SHIM_SLOG2_H

#define SLOG2_ERROR     2
#define SLOG2_WARNING   3
#define SLOG2_INFO      5

#endif
