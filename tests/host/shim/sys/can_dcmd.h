/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests only. A stand-in for the names from QNX's <sys/can_dcmd.h>
 * that src/ctucanfd_hw.c and src/dev-can-ctucanfd.c use, so they build with
 * the host compiler. It is written from how this repository uses them, not
 * copied from the QNX header, and its layout and command values are not QNX's.
 * Never put this directory on the include path of a QNX build.
 */
#ifndef CTUCAN_HOST_SHIM_CAN_DCMD_H
#define CTUCAN_HOST_SHIM_CAN_DCMD_H

#include <stdint.h>
#include <devctl.h>

#define CAN_MSG_DATA_MAX_CAN    8
#define CAN_MSG_DATA_MAX_CANFD  64
#define CAN_MSG_DATA_MAX        CAN_MSG_DATA_MAX_CANFD
#define CAN_MSG_MID_UNKNOWN     0xffffffffu     /* plain write(): the mailbox picks the id */

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

typedef struct {
    char     description[64];
    uint32_t msgq_size, waitq_size, mode, bit_rate;
    uint32_t num_tx_mboxes, num_rx_mboxes;
    uint32_t loopback_internal, autobus_on, silent;
    uint16_t bit_rate_prescaler;
    uint8_t  sync_jump_width, time_segment_1, time_segment_2;
} CAN_DEVCTL_INFO;

typedef struct {
    uint32_t transmitted_frames, received_frames;
    uint32_t total_frame_errors, sw_receive_q_full, hw_receive_overflows;
    uint32_t bus_off_state_count, error_warning_state_count, error_passive_state_count;
    uint32_t dom_bit_recess_errors, crc_errors, form_errors, missing_ack, stuff_errors;
    uint32_t total_interrupts, tx_interrupts, rx_interrupts;
} CAN_DEVCTL_STATS;

typedef struct {
    uint32_t drvr1, drvr2, drvr3, drvr4;
} CAN_DEVCTL_ERROR;

typedef struct {
    uint32_t ref_clock_freq;
    uint16_t bit_rate_prescaler;
    uint8_t  sync_jump_width, time_segment_1, time_segment_2;
} CAN_DEVCTL_TIMING;

typedef union {
    uint32_t          mid, mfilter, timestamp;
    CAN_DEVCTL_INFO   info;
    CAN_DEVCTL_STATS  stats;
    CAN_DEVCTL_ERROR  error;
    CAN_DEVCTL_TIMING timing;
    CAN_MSG           canmsg;
    uint8_t           room[128];  /* the driver asserts its private replies fit */
} CAN_DCMD_DATA;

#define _DCMD_CAN   0x0c

#define CAN_DEVCTL_GET_MID              __DIOF(_DCMD_CAN, 1, uint32_t)
#define CAN_DEVCTL_SET_MID              __DIOT(_DCMD_CAN, 2, uint32_t)
#define CAN_DEVCTL_GET_MFILTER          __DIOF(_DCMD_CAN, 3, uint32_t)
#define CAN_DEVCTL_SET_MFILTER          __DIOT(_DCMD_CAN, 4, uint32_t)
#define CAN_DEVCTL_GET_TIMESTAMP        __DIOF(_DCMD_CAN, 5, uint32_t)
#define CAN_DEVCTL_SET_TIMESTAMP        __DIOT(_DCMD_CAN, 6, uint32_t)
#define CAN_DEVCTL_GET_INFO             __DIOF(_DCMD_CAN, 7, CAN_DEVCTL_INFO)
#define CAN_DEVCTL_GET_STATS            __DIOF(_DCMD_CAN, 8, CAN_DEVCTL_STATS)
#define CAN_DEVCTL_ERROR                __DIOF(_DCMD_CAN, 9, CAN_DEVCTL_ERROR)
#define CAN_DEVCTL_SET_TIMING           __DIOT(_DCMD_CAN, 10, CAN_DEVCTL_TIMING)
#define CAN_DEVCTL_DEBUG_INFO           __DION(_DCMD_CAN, 11)
#define CAN_DEVCTL_DEBUG_INFO2          __DION(_DCMD_CAN, 12)
#define CAN_DEVCTL_WRITE_CANMSG_EXT     __DIOT(_DCMD_CAN, 13, CAN_MSG)
#define CAN_DEVCTL_READ_CANMSG_EXT      __DIOF(_DCMD_CAN, 14, CAN_MSG)
#define CAN_DEVCTL_TX_FRAME_RAW         __DIOT(_DCMD_CAN, 15, CAN_MSG)
#define CAN_DEVCTL_RX_FRAME_RAW_BLOCK   __DIOF(_DCMD_CAN, 16, CAN_MSG)
#define CAN_DEVCTL_RX_FRAME_RAW_NOBLOCK __DIOF(_DCMD_CAN, 17, CAN_MSG)

#endif
