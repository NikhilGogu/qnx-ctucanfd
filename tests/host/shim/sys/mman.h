/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests only. The host's <sys/mman.h> plus the two device mapping
 * calls and the PROT_NOCACHE flag QNX adds, which src/dev-can-ctucanfd.c
 * uses. Written from that use, not copied from QNX's header.
 * tests/host/fake_libcan.c implements the calls on the fake core. Never put
 * this directory on the include path of a QNX build.
 */
#ifndef CTUCAN_HOST_SHIM_MMAN_H
#define CTUCAN_HOST_SHIM_MMAN_H

#include_next <sys/mman.h>
#include <stddef.h>
#include <stdint.h>

#define PROT_NOCACHE    0

void *mmap_device_memory(void *addr, size_t len, int prot, int flags, uint64_t physical);
int   munmap_device_memory(void *addr, size_t len);

#endif
