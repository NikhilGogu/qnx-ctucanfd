/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hardware layer for one CTU CAN FD core. No libcan or OS state in here, so
 * the same code can back a resource manager, a test tool or a unit test.
 *
 * Built with -DCTUCAN_FI, every register access goes through the fault
 * injector in ctucanfd_fi.c, which overlays scripted error-state and
 * TX-completion sequences on the real registers (see docs/DESIGN.md).
 */
#ifndef CTUCANFD_HW_H
#define CTUCANFD_HW_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/can_dcmd.h>
#include "ctucanfd_regs.h"

struct ctucan_fi;

typedef struct {
    volatile uint8_t *base;     /* mapped core registers */
    struct ctucan_fi *fi;       /* fault injector; NULL unless built with CTUCAN_FI */
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

static inline uint32_t ctucan_raw_rd(const ctucan_hw_t *hw, uint32_t off)
{
    return *(volatile uint32_t *)(hw->base + off);
}

static inline void ctucan_raw_wr(const ctucan_hw_t *hw, uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(hw->base + off) = val;
}

#ifdef CTUCAN_FI
uint32_t ctucan_rd(const ctucan_hw_t *hw, uint32_t off);
void     ctucan_wr(const ctucan_hw_t *hw, uint32_t off, uint32_t val);
#else
static inline uint32_t ctucan_rd(const ctucan_hw_t *hw, uint32_t off)
{
    return ctucan_raw_rd(hw, off);
}

static inline void ctucan_wr(const ctucan_hw_t *hw, uint32_t off, uint32_t val)
{
    ctucan_raw_wr(hw, off, val);
}
#endif

/* Datasheet 2.9: a TXT buffer may be written only in these states. */
static inline bool ctucan_txt_writable(unsigned st)
{
    return st == TXT_ETY || st == TXT_TOK || st == TXT_ERR || st == TXT_ABT || st == TXT_PER;
}

int      ctucan_hw_identify(const ctucan_hw_t *hw, unsigned *major, unsigned *minor);
int      ctucan_hw_reset(const ctucan_hw_t *hw);
void     ctucan_hw_start(const ctucan_hw_t *hw, const ctucan_hw_cfg_t *cfg, uint32_t int_ena);
void     ctucan_hw_stop(const ctucan_hw_t *hw);

unsigned ctucan_hw_txt_state(const ctucan_hw_t *hw, unsigned buf);
int      ctucan_hw_txt_write(const ctucan_hw_t *hw, unsigned buf, const CAN_MSG *msg, bool fdf, bool brs);
void     ctucan_hw_txt_cmd(const ctucan_hw_t *hw, uint32_t cmd, unsigned bufmask);
void     ctucan_hw_tx_prio(const ctucan_hw_t *hw, unsigned ntxb, unsigned oldest);

unsigned ctucan_hw_rx_frames(const ctucan_hw_t *hw);
int      ctucan_hw_rx(const ctucan_hw_t *hw, CAN_MSG *msg, ctucan_flags_t *fl);
void     ctucan_hw_rx_flush(const ctucan_hw_t *hw);

unsigned ctucan_len2dlc(unsigned len);
unsigned ctucan_dlc2len(unsigned dlc, bool fdf);
int      ctucan_bt_calc(uint32_t clk_hz, uint32_t bitrate, unsigned sp_permille, bool data_phase,
                        ctucan_bt_t *bt);
int      ctucan_bt_check(const ctucan_bt_t *bt, bool data_phase);
uint32_t ctucan_bt_reg(const ctucan_bt_t *bt);

#ifdef CTUCAN_FI
enum {
    CTUCAN_FI_OFF,
    CTUCAN_FI_WARN,         /* TEC crosses the warning limit and back; FAULT_STATE stays active */
    CTUCAN_FI_PASSIVE,      /* warning, error passive, back to error active */
    CTUCAN_FI_BUSOFF,       /* warning, passive, bus-off; recovers after the driver's ERCRST */
    CTUCAN_FI_TXDELAY,      /* TX completions held back: arg ms per frame, arg2 frames */
    CTUCAN_FI_RXCORRUPT,    /* format word of RX frame arg2 altered: arg 1 = RWCNT one short, 2 = RWCNT 31 */
};

struct ctucan_fi *ctucan_fi_create(void);
int      ctucan_fi_start(struct ctucan_fi *fi, unsigned scenario, unsigned arg, unsigned arg2, uint64_t now_ms);
bool     ctucan_fi_tick(struct ctucan_fi *fi, uint64_t now_ms);
unsigned ctucan_fi_state(const struct ctucan_fi *fi);
#endif

#endif
