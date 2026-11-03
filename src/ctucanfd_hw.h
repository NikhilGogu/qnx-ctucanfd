/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hardware layer for one CTU CAN FD core. No libcan or OS state in here, so
 * the same code can back a resource manager, a test tool or a unit test.
 */
#ifndef CTUCANFD_HW_H
#define CTUCANFD_HW_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/can_dcmd.h>
#include "ctucanfd_regs.h"

typedef struct {
    volatile uint8_t *base;     /* mapped core registers */
} ctucan_hw_t;

typedef struct {
    uint16_t brp;
    uint8_t  prop, ph1, ph2, sjw;
} ctucan_bt_t;

typedef struct {
    bool fd;                    /* MODE.FDE */
    bool non_iso;               /* SETTINGS.NISOFD */
    bool loopback;              /* SETTINGS.ILBP */
    bool listen_only;           /* MODE.BMM */
    ctucan_bt_t nominal;
    ctucan_bt_t data;
} ctucan_hw_cfg_t;

typedef struct {
    bool fdf, brs, esi;
} ctucan_flags_t;

static inline uint32_t ctucan_rd(const ctucan_hw_t *hw, uint32_t off)
{
    return *(volatile uint32_t *)(hw->base + off);
}

static inline void ctucan_wr(const ctucan_hw_t *hw, uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(hw->base + off) = val;
}

int      ctucan_hw_identify(const ctucan_hw_t *hw, unsigned *major, unsigned *minor);
void     ctucan_hw_reset(const ctucan_hw_t *hw);
void     ctucan_hw_start(const ctucan_hw_t *hw, const ctucan_hw_cfg_t *cfg, uint32_t int_ena);
void     ctucan_hw_stop(const ctucan_hw_t *hw);
int      ctucan_hw_tx(const ctucan_hw_t *hw, unsigned buf, const CAN_MSG *msg, bool fdf, bool brs);
unsigned ctucan_hw_rx_frames(const ctucan_hw_t *hw);
int      ctucan_hw_rx(const ctucan_hw_t *hw, CAN_MSG *msg, ctucan_flags_t *fl);

unsigned ctucan_len2dlc(unsigned len);
unsigned ctucan_dlc2len(unsigned dlc, bool fdf);
int      ctucan_bt_calc(uint32_t clk_hz, uint32_t bitrate, unsigned sp_permille, bool data_phase,
                        ctucan_bt_t *bt);
uint32_t ctucan_bt_reg(const ctucan_bt_t *bt);

#endif
