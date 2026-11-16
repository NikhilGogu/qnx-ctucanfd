/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests only. A stand-in for the names from QNX's <devctl.h> that
 * this repository uses. It is written from how the repository uses them, not
 * copied from the QNX header; the command encoding here only has to give
 * distinct numbers and is not QNX's. Never put this directory on the include
 * path of a QNX build.
 */
#ifndef CTUCAN_HOST_SHIM_DEVCTL_H
#define CTUCAN_HOST_SHIM_DEVCTL_H

#include <stddef.h>

#define _DCMD_MISC  0x05

#define __DION(class, cmd)          ((int)(((class) << 8) | (cmd)))
#define __DIOF(class, cmd, data)    ((int)((1u << 30) | ((class) << 8) | (cmd)))
#define __DIOT(class, cmd, data)    ((int)((2u << 28) | ((class) << 8) | (cmd)))

int devctl(int fd, int dcmd, void *data, size_t nbytes, int *info);

#endif
