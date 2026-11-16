/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests only. A stand-in for the few names from QNX's
 * <sys/can_dcmd.h> that src/ctucanfd_hw.c uses, so the hardware layer builds
 * with the host compiler. It is written from how this repository uses them,
 * not copied from the QNX header, and its layout is not QNX's. Never put this
 * directory on the include path of a QNX build.
 */
#ifndef CTUCAN_HOST_SHIM_CAN_DCMD_H
#define CTUCAN_HOST_SHIM_CAN_DCMD_H

#include <stdint.h>

#define CAN_MSG_DATA_MAX_CAN    8
#define CAN_MSG_DATA_MAX_CANFD  64
#define CAN_MSG_DATA_MAX        CAN_MSG_DATA_MAX_CANFD

typedef struct {
    uint32_t timestamp;
    uint32_t is_extended_mid;
    uint32_t is_remote_frame;
} CAN_MSG_EXT;

typedef struct {
    uint8_t     dat[CAN_MSG_DATA_MAX];
    uint8_t     len;
    uint32_t    mid;
    CAN_MSG_EXT ext;
} CAN_MSG;

#endif
