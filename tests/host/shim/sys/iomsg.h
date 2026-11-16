/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests only. A stand-in for the devctl message names from QNX's
 * <sys/iomsg.h> that src/dev-can-ctucanfd.c uses. It is written from how the
 * driver uses them, not copied from the QNX header, and its layout is not
 * QNX's: here the payload simply follows the header. Never put this directory
 * on the include path of a QNX build.
 */
#ifndef CTUCAN_HOST_SHIM_IOMSG_H
#define CTUCAN_HOST_SHIM_IOMSG_H

#include <stdint.h>

typedef union {
    struct {
        uint32_t dcmd;
        uint32_t nbytes;    /* bytes the client sent with the command */
    } i;
    uint64_t align;
} io_devctl_t;

#define _IO_INPUT_PAYLOAD(msg)  ((void *)((msg) + 1))
#define _IO_OUTPUT_PAYLOAD(msg) ((void *)((msg) + 1))

#endif
