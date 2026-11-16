/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests only. A stand-in for the kernel calls from QNX's
 * <sys/neutrino.h> that src/dev-can-ctucanfd.c uses. It is written from how
 * the driver uses them, not copied from the QNX header; the values are not
 * QNX's. tests/host/fake_libcan.c implements the calls. Never put this
 * directory on the include path of a QNX build.
 */
#ifndef CTUCAN_HOST_SHIM_NEUTRINO_H
#define CTUCAN_HOST_SHIM_NEUTRINO_H

#define _PULSE_CODE_MINAVAIL    0
#define _NTO_TCTL_IO_LEVEL      1
#define _NTO_IO_LEVEL_1         1

int InterruptUnmask(int intr, int id);
int ThreadCtl(int cmd, void *data);

#endif
