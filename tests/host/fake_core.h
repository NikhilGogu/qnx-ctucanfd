/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * A register-level stand-in for one CTU CAN FD core, for the host unit tests.
 * src/ctucanfd_hw.c is built with -DCTUCAN_HW_EXTERN_IO, so every register
 * access goes through ctucan_rd() and ctucan_wr() in fake_core.c.
 *
 * It models only what the hardware layer relies on, as docs/DESIGN.md and
 * src/ctucanfd_regs.h describe it: the RX FIFO popped through RX_DATA with
 * RX_MEM_INFO and RX_STATUS derived from it, TXT buffer memory, TX_STATUS
 * driven by TX_COMMAND, INT_ENA set and clear, INT_STAT cleared by writing 1,
 * the MODE word and a soft reset.
 * It is not a model of the silicon or of QEMU's ctucan_core.c.
 */
#ifndef CTUCAN_HOST_FAKE_CORE_H
#define CTUCAN_HOST_FAKE_CORE_H

#include <stdbool.h>
#include <stdint.h>
#include "ctucanfd_hw.h"

#define FAKE_RX_WORDS   0x800u      /* RX FIFO size in words, as QEMU 6.2 reports it */
#define FAKE_LOG_MAX    4096u

typedef struct {
    uint32_t off, val;
} fake_wr_t;

typedef struct {
    uint32_t mem[CTUCAN_CORE_SPAN / 4];     /* plain registers and TXT buffer memory */
    uint32_t rx[FAKE_RX_WORDS];
    unsigned rx_head, rx_used, rx_frames;
    unsigned txt[CTUCAN_TXT_BUFFERS];       /* TXT_* state per buffer */
    uint32_t int_ena;
    uint32_t int_storm;                     /* INT_STAT bits set again as soon as they are cleared */
    uint32_t device_id;                     /* what DEVICE_ID reads */
    uint32_t mode_reset;                    /* MODE/SETTINGS after power-on and soft reset; fake_init() sets
                                             * MODE_FDE, the value QEMU 6.2 shows */
    bool     reset_stuck;                   /* a soft reset never completes */
    bool     posted;                        /* writes land at the next read, as PCI allows */
    bool     abort_slow;                    /* an abort leaves the buffer "abort in progress" (TXT_ABTP) */
    unsigned rx_reads;                      /* RX_DATA words popped */
    fake_wr_t log[FAKE_LOG_MAX];            /* register writes, in order */
    unsigned nlog;
    /* Observers for test_txw_orders.c, NULL unless a test sets them (fake_init()
     * clears them): on_rd sees every value read, on_wr every write before it
     * takes effect. They watch only; nothing about the core changes. */
    void   (*on_rd)(uint32_t off, uint32_t val);
    void   (*on_wr)(uint32_t off, uint32_t val);
} fake_core_t;

extern fake_core_t fake;

/* Set by a test before it starts the driver, and kept by fake_init(). While true: the soft reset value
 * of MODE/SETTINGS has TBFBO = 1; writing SETTINGS.ENA makes FAULT_STATE read bus-off (the core is
 * integrating) until the test writes error active; and a Set ready command while FAULT_STATE reads
 * bus-off with TBFBO = 1 moves the TXT buffer to Aborted (datasheet 2.9.7, note). While false the core
 * behaves as before: FAULT_STATE reads what fake_fault_reset says. */
extern bool fake_integrate;

/* What FAULT_STATE (with the warning limit in its low bits) reads after a soft reset. Kept by fake_init(),
 * like fake_integrate. Default 0: none of error active, error passive and bus-off, as QEMU 6.2 reads it
 * (docs/DESIGN.md, section 1). A test may set FS_ERA | 96 or FS_ERP | 96 before it starts the driver. */
extern uint32_t fake_fault_reset;

/* What DEVICE_ID reads after fake_init(), when not 0: a test sets it before it starts the driver to put
 * something that is not a CTU CAN FD core behind the BAR. Kept by fake_init(), like fake_integrate. */
extern uint32_t fake_device_id_force;

/* Power-on state: reset values, empty FIFO, four empty TXT buffers. */
void fake_init(ctucan_hw_t *hw);

/* Appends one frame (format word first) to the RX FIFO and sets RBNEI.
 * `words` counts every word including the format word. */
void fake_rx_push(const uint32_t *w, unsigned words);

/* The word last written to `off` through ctucan_wr(), or 0. */
uint32_t fake_last_write(uint32_t off);

/* How many writes went to `off`. */
unsigned fake_write_count(uint32_t off);

#endif
