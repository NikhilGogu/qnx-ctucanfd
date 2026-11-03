/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <string.h>
#include "ctucanfd_hw.h"

static const uint8_t dlc_len[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64 };

unsigned ctucan_dlc2len(unsigned dlc, bool fdf)
{
    dlc &= FFW_DLC_MASK;
    if (!fdf) {
        return dlc > 8 ? 8 : dlc;   /* classic DLC 9..15 still carries 8 bytes */
    }
    return dlc_len[dlc];
}

unsigned ctucan_len2dlc(unsigned len)
{
    if (len <= 8) {
        return len;
    }
    for (unsigned dlc = 9; dlc < 15; dlc++) {
        if (len <= dlc_len[dlc]) {
            return dlc;
        }
    }
    return 15;
}

int ctucan_hw_identify(const ctucan_hw_t *hw, unsigned *major, unsigned *minor)
{
    uint32_t v = ctucan_rd(hw, CTUCAN_DEVICE_ID);

    if ((v & 0xffffu) != CTUCAN_ID_VALUE) {
        return ENODEV;
    }
    *major = (v >> 24) & 0xffu;
    *minor = (v >> 16) & 0xffu;
    return EOK;
}

int ctucan_hw_reset(const ctucan_hw_t *hw)
{
    ctucan_wr(hw, CTUCAN_MODE, MODE_RST);
    /* The soft reset is synchronous on the IP core; wait until the register
     * bus answers with reset values again. */
    for (int i = 0; i < 10000; i++) {
        if (ctucan_rd(hw, CTUCAN_YOLO) == CTUCAN_YOLO_VALUE &&
            (ctucan_rd(hw, CTUCAN_DEVICE_ID) & 0xffffu) == CTUCAN_ID_VALUE &&
            (ctucan_rd(hw, CTUCAN_MODE) & (MODE_RST | SET_ENA)) == 0) {
            return EOK;
        }
    }
    return ETIMEDOUT;
}

void ctucan_hw_start(const ctucan_hw_t *hw, const ctucan_hw_cfg_t *cfg, uint32_t int_ena)
{
    uint32_t mode = SET_ENA;

    ctucan_wr(hw, CTUCAN_INT_ENA_CLR, INT_ALL);
    /* Sources the driver does not service never latch in INT_STAT. */
    ctucan_wr(hw, CTUCAN_INT_MASK_SET, INT_ALL & ~int_ena);
    ctucan_wr(hw, CTUCAN_INT_MASK_CLR, int_ena);
    ctucan_wr(hw, CTUCAN_INT_STAT, INT_ALL);
    ctucan_wr(hw, CTUCAN_BTR, ctucan_bt_reg(&cfg->nominal));
    ctucan_wr(hw, CTUCAN_BTR_FD, ctucan_bt_reg(&cfg->data));
    ctucan_wr(hw, CTUCAN_COMMAND, CMD_RRB | CMD_CDO | CMD_RXFCRST | CMD_TXFCRST);

    if (cfg->fd) {
        mode |= MODE_FDE;
    }
    if (cfg->listen_only) {
        mode |= MODE_BMM;
    }
    if (cfg->loopback) {
        mode |= SET_ILBP;
    }
    if (cfg->non_iso) {
        mode |= SET_NISOFD;
    }
    ctucan_wr(hw, CTUCAN_MODE, mode);
    ctucan_wr(hw, CTUCAN_INT_ENA_SET, int_ena);
}

void ctucan_hw_stop(const ctucan_hw_t *hw)
{
    ctucan_wr(hw, CTUCAN_INT_ENA_CLR, INT_ALL);
    ctucan_wr(hw, CTUCAN_MODE, 0);      /* SETTINGS.ENA = 0: the core leaves the bus */
    ctucan_wr(hw, CTUCAN_INT_STAT, INT_ALL);
}

unsigned ctucan_hw_txt_state(const ctucan_hw_t *hw, unsigned buf)
{
    return TXT_STATE(ctucan_rd(hw, CTUCAN_TX_STATUS), buf);
}

/* Fills TXT buffer `buf`; the caller sets it ready with ctucan_hw_txt_cmd(). */
int ctucan_hw_txt_write(const ctucan_hw_t *hw, unsigned buf, const CAN_MSG *msg, bool fdf, bool brs)
{
    unsigned len = msg->len;
    bool rtr = msg->ext.is_remote_frame != 0;

    if (buf >= CTUCAN_TXT_BUFFERS || len > (fdf ? CAN_MSG_DATA_MAX_CANFD : CAN_MSG_DATA_MAX_CAN) ||
        (rtr && fdf)) {
        return EINVAL;  /* CAN FD has no remote frames */
    }
    if (!ctucan_txt_writable(ctucan_hw_txt_state(hw, buf))) {
        return EBUSY;
    }

    /* CAN FD lengths are quantised (12, 16, 20, 24, 32, 48, 64): round the
     * DLC up and pad with zeros. A remote frame carries a DLC but no data. */
    unsigned dlc = fdf ? ctucan_len2dlc(len) : len;
    unsigned words = rtr ? 0 : (ctucan_dlc2len(dlc, fdf) + 3u) / 4u;
    uint32_t ffw = dlc | ((3u + words) << FFW_RWCNT_SHIFT);
    uint32_t idw;

    if (msg->ext.is_extended_mid) {
        ffw |= FFW_IDE;
        idw = msg->mid & IDW_EXT_MASK;
    } else {
        idw = msg->mid & IDW_STD_MASK;
    }
    if (fdf) {
        ffw |= FFW_FDF;
        if (brs) {
            ffw |= FFW_BRS;
        }
    } else if (rtr) {
        ffw |= FFW_RTR;
    }

    uint32_t tb = CTUCAN_TXTB(buf);
    ctucan_wr(hw, tb + FRAME_FFW, ffw);
    ctucan_wr(hw, tb + FRAME_IDW, idw);
    ctucan_wr(hw, tb + FRAME_TSL, 0);   /* 0 = send now (no time-triggered TX) */
    ctucan_wr(hw, tb + FRAME_TSU, 0);
    for (unsigned w = 0; w < words; w++) {
        uint32_t v = 0;
        for (unsigned b = 0; b < 4; b++) {
            unsigned i = w * 4u + b;
            if (i < len) {
                v |= (uint32_t)msg->dat[i] << (8u * b);
            }
        }
        ctucan_wr(hw, tb + FRAME_DATA + 4u * w, v);
    }
    return EOK;
}

void ctucan_hw_txt_cmd(const ctucan_hw_t *hw, uint32_t cmd, unsigned bufmask)
{
    ctucan_wr(hw, CTUCAN_TX_COMMAND, cmd | TXC_TXB_MASK(bufmask));
}

/* The core sends the ready buffer with the highest priority first. Buffer
 * `oldest` gets the top priority and the next ones in order get lower ones,
 * so ready buffers leave in FIFO order. QEMU 6.2 never sends a buffer with
 * priority 0, so priorities run from 7 down. */
void ctucan_hw_tx_prio(const ctucan_hw_t *hw, unsigned ntxb, unsigned oldest)
{
    uint32_t v = 0;

    for (unsigned k = 0; k < ntxb; k++) {
        v |= (7u - k) << (4u * ((oldest + k) % ntxb));
    }
    ctucan_wr(hw, CTUCAN_TX_PRIORITY, v);
}

unsigned ctucan_hw_rx_frames(const ctucan_hw_t *hw)
{
    return RXST_RXFRC(ctucan_rd(hw, CTUCAN_RX_STATUS));
}

void ctucan_hw_rx_flush(const ctucan_hw_t *hw)
{
    ctucan_wr(hw, CTUCAN_COMMAND, CMD_RRB);
}

/*
 * Pops one frame. The format word is checked against itself before anything
 * else is read: RWCNT must be what its DLC and frame format need (a remote
 * frame: 3, or 3 plus the data words as QEMU 6.2 stores it) and no more than
 * the FIFO holds. On any mismatch nothing more is consumed and EIO is
 * returned; the caller must then flush the FIFO, because where the next frame
 * starts is unknown and its payload could pass for a format word.
 */
int ctucan_hw_rx(const ctucan_hw_t *hw, CAN_MSG *msg, ctucan_flags_t *fl)
{
    uint32_t mi = ctucan_rd(hw, CTUCAN_RX_MEM_INFO);
    unsigned used = RXMI_SIZE(mi) - RXMI_FREE(mi);
    uint32_t ffw = ctucan_rd(hw, CTUCAN_RX_DATA);
    unsigned rwcnt = (ffw >> FFW_RWCNT_SHIFT) & FFW_RWCNT_MASK;
    bool fdf = (ffw & FFW_FDF) != 0;
    bool rtr = (ffw & FFW_RTR) != 0;
    unsigned len = ctucan_dlc2len(ffw & FFW_DLC_MASK, fdf);
    unsigned dwords = (len + 3u) / 4u;
    uint32_t w[3 + 16];
    bool ok;

    if (rtr) {
        ok = !fdf && (rwcnt == 3 || rwcnt == 3 + dwords);   /* CAN FD has no remote frames */
    } else {
        ok = rwcnt == 3 + dwords;
    }
    if (!fdf && (ffw & (FFW_BRS | FFW_ESI)) != 0) {
        ok = false;     /* bit rate switch and ESI exist only in CAN FD frames */
    }
    if (!ok || rwcnt + 1 > used) {
        return EIO;
    }
    for (unsigned i = 0; i < rwcnt; i++) {
        w[i] = ctucan_rd(hw, CTUCAN_RX_DATA);
    }

    memset(msg, 0, sizeof(*msg));
    msg->len = (uint8_t)len;
    if (ffw & FFW_IDE) {
        msg->mid = w[0] & IDW_EXT_MASK;
        msg->ext.is_extended_mid = 1;
    } else {
        msg->mid = w[0] & IDW_STD_MASK;
    }
    msg->ext.is_remote_frame = rtr;
    if (!rtr) {
        for (unsigned i = 0; i < len; i++) {
            msg->dat[i] = (uint8_t)(w[3u + i / 4u] >> (8u * (i % 4u)));
        }
    }
    if (fl != NULL) {
        fl->fdf = fdf;
        fl->brs = (ffw & FFW_BRS) != 0;
        fl->esi = (ffw & FFW_ESI) != 0;
    }
    return EOK;
}

int ctucan_bt_check(const ctucan_bt_t *bt, bool data_phase)
{
    const unsigned prop_max = data_phase ? 63u : 127u;
    const unsigned ph_max = data_phase ? 31u : 63u;

    if (bt->brp < 1 || bt->brp > 255 || bt->prop > prop_max || bt->ph1 > ph_max || bt->ph2 < 1 ||
        bt->ph2 > ph_max || bt->prop + bt->ph1 < 2u) {
        return EINVAL;
    }
    /* ISO 11898-1: the resynchronisation jump width may not exceed either phase segment. */
    if (bt->sjw < 1 || bt->sjw > 31 || bt->sjw > bt->ph1 || bt->sjw > bt->ph2) {
        return EINVAL;
    }
    if (1u + bt->prop + bt->ph1 + bt->ph2 < 8u) {
        return EINVAL;
    }
    return EOK;
}

int ctucan_bt_calc(uint32_t clk_hz, uint32_t bitrate, unsigned sp_permille, bool data_phase,
                   ctucan_bt_t *bt)
{
    const unsigned prop_max = data_phase ? 63u : 127u;
    const unsigned ph1_max = data_phase ? 31u : 63u;
    const unsigned ph2_max = data_phase ? 31u : 63u;
    const unsigned sjw_max = 31u;

    if (clk_hz == 0 || bitrate == 0 || sp_permille < 500 || sp_permille > 950) {
        return EINVAL;
    }
    /* First pass prefers at most 50 time quanta per bit, second takes any. */
    for (int pass = 0; pass < 2; pass++) {
        for (unsigned brp = 1; brp <= 255; brp++) {
            uint64_t q = (uint64_t)brp * bitrate;
            if (clk_hz % q != 0) {
                continue;
            }
            unsigned ntq = (unsigned)(clk_hz / q);
            if (ntq < 8 || ntq > 1u + prop_max + ph1_max + ph2_max || (pass == 0 && ntq > 50)) {
                continue;
            }
            unsigned tseg1 = (ntq * sp_permille + 500u) / 1000u - 1u;   /* after the sync quantum */
            unsigned ph2 = ntq - 1u - tseg1;
            if (tseg1 < 2 || ph2 < 1 || ph2 > ph2_max) {
                continue;
            }
            unsigned ph1 = tseg1 / 2u;
            if (ph1 > ph1_max) {
                ph1 = ph1_max;
            }
            unsigned prop = tseg1 - ph1;
            if (prop > prop_max) {
                continue;
            }
            unsigned sjw = ph1 < ph2 ? ph1 : ph2;
            bt->brp = (uint16_t)brp;
            bt->prop = (uint8_t)prop;
            bt->ph1 = (uint8_t)ph1;
            bt->ph2 = (uint8_t)ph2;
            bt->sjw = (uint8_t)(sjw < sjw_max ? sjw : sjw_max);
            return ctucan_bt_check(bt, data_phase);
        }
    }
    return EINVAL;
}

uint32_t ctucan_bt_reg(const ctucan_bt_t *bt)
{
    return ((uint32_t)bt->prop << BTR_PROP_SHIFT) | ((uint32_t)bt->ph1 << BTR_PH1_SHIFT) |
           ((uint32_t)bt->ph2 << BTR_PH2_SHIFT) | ((uint32_t)bt->brp << BTR_BRP_SHIFT) |
           ((uint32_t)bt->sjw << BTR_SJW_SHIFT);
}
