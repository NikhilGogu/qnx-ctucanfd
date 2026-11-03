/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Driver-specific devctls of dev-can-ctucanfd, shared with the tools. The
 * standard CAN devctls are in <sys/can_dcmd.h>.
 */
#ifndef CTUCANFD_DEVCTL_H
#define CTUCANFD_DEVCTL_H

#include <stdint.h>
#include <devctl.h>
#include <sys/can_dcmd.h>

/* The pulse code the driver registers with libcan for its interrupt. A
 * client that sends it makes the driver unmask an interrupt; the kernel
 * refuses when nothing is masked and the driver counts it in unmask_err. */
#define CTUCAN_PULSE_IRQ    0

typedef struct {
    uint32_t hw_pulses;         /* interrupt pulses */
    uint32_t deferred;          /* pulses that ended with work left for the next interrupt */
    uint32_t unmask_err;        /* InterruptUnmask() failures */
    uint32_t rearm_pending;     /* status pending when the core's interrupt output was re-enabled */
    uint32_t spurious;          /* interrupt pulses with nothing pending in the core */
    uint32_t reports;           /* RX mailboxes returned to libcan to wake a reader */
    uint32_t notify_deferred;   /* pulses that left reader notifications for the next one (RBNEI held) */
    uint32_t rx_max_batch;      /* most frames moved in one pulse */
    uint32_t rx_flushes;        /* FIFO flushes: a format word did not match its frame */
    uint32_t rx_flushed;        /* frames discarded by those flushes */
    uint32_t tx_started;
    uint32_t tx_ok;
    uint32_t tx_failed;         /* TXT buffer ended in "TX failed" */
    uint32_t tx_aborted;        /* aborted by the driver: bus-off, restart or shutdown */
    uint32_t tx_lost;           /* TXT buffer found empty while its frame was in flight */
    uint32_t tx_rejected;       /* frames refused at the hardware (format), dropped */
    uint32_t tx_hw_busy;        /* next TXT buffer not writable when a frame was due */
    uint32_t tx_order_err;      /* a later TXT buffer finished before an earlier one */
    uint32_t tx_quiesce_timeout;
    uint32_t tx_max_inflight;   /* most frames in TXT buffers at once */
    uint32_t tx_batches;        /* TX_COMMAND writes that set several buffers ready at once */
    uint32_t ewl_up;            /* warning limit reached */
    uint32_t ewl_down;          /* back below the warning limit */
    uint32_t busoff_recovered;  /* bus-off left, confirmed by FAULT_STATE = error active */
    uint32_t restarts;          /* controller restarts (SET_TIMING) that completed */
    uint32_t restart_fail;      /* restarts that failed because the core did not come out of reset */
    uint32_t tx_abort_forced;   /* aborted buffers that did not reach a final state in time */
    uint32_t state;             /* [31:28] FI scenario, [27:24] FI step, [18:16] FAULT_STATE,
                                 * bit 9 bus-off hold, bit 8 STATUS.EWL, [7:0] frames in flight */
} ctucan_xstats_t;

typedef struct {
    uint32_t scenario;          /* CTUCAN_FI_* in src/ctucanfd_hw.h */
    uint32_t arg, arg2;         /* scenario arguments, see ctucanfd_fi.c */
    uint32_t budget;            /* RX frames per pulse, 0 = default */
    uint32_t flags;
} ctucan_fi_req_t;

#define CTUCAN_FI_F_FIXEDPRIO   2u  /* stage-one fixed TXT priorities (buffer n gets n + 1): negative control */
#define CTUCAN_FI_F_REPORT1     4u  /* one RX mailbox return per pulse */

#define CTUCAN_DCMD_CODE        200
#define CTUCAN_DEVCTL_GET_XSTATS (__DIOF(_DCMD_MISC, CTUCAN_DCMD_CODE + 0, ctucan_xstats_t))
#define CTUCAN_DEVCTL_RECOVER    (__DION(_DCMD_MISC, CTUCAN_DCMD_CODE + 1))
/* fault-injection builds only; the normal build answers them like any unknown command */
#define CTUCAN_DEVCTL_FI         (__DIOT(_DCMD_MISC, CTUCAN_DCMD_CODE + 2, ctucan_fi_req_t))
#define CTUCAN_DEVCTL_FI_TICK    (__DION(_DCMD_MISC, CTUCAN_DCMD_CODE + 3))

#endif
