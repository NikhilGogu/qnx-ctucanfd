/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include "fake_core.h"

fake_core_t fake;

static unsigned rx_len[FAKE_RX_WORDS];     /* words of each frame in the FIFO, oldest first */
static unsigned rx_nlen, rx_left;          /* frames queued; words left of the frame being read */

static void fake_reset(void)
{
    memset(fake.mem, 0, sizeof(fake.mem));
    fake.mem[CTUCAN_MODE / 4] = MODE_FDE;  /* reset value QEMU 6.2 shows (docs/DESIGN.md, section 1) */
    fake.rx_head = fake.rx_used = fake.rx_frames = 0;
    rx_nlen = rx_left = 0;
    for (unsigned b = 0; b < CTUCAN_TXT_BUFFERS; b++) {
        fake.txt[b] = TXT_ETY;
    }
    fake.int_ena = 0;
}

void fake_init(ctucan_hw_t *hw)
{
    memset(&fake, 0, sizeof(fake));
    fake_reset();
    fake.device_id = 0x0202u << 16 | CTUCAN_ID_VALUE;   /* version 2.2 */
    hw->base = (volatile uint8_t *)fake.mem;
    hw->fi = NULL;
}

void fake_rx_push(const uint32_t *w, unsigned words)
{
    for (unsigned i = 0; i < words && fake.rx_used < FAKE_RX_WORDS; i++) {
        fake.rx[(fake.rx_head + fake.rx_used) % FAKE_RX_WORDS] = w[i];
        fake.rx_used++;
    }
    rx_len[rx_nlen++] = words;
    fake.rx_frames++;
}

static uint32_t rx_pop(void)
{
    if (fake.rx_used == 0) {
        return 0;
    }
    if (rx_left == 0 && rx_nlen > 0) {
        rx_left = rx_len[0];
        memmove(rx_len, rx_len + 1, (rx_nlen - 1) * sizeof(rx_len[0]));
        rx_nlen--;
    }
    uint32_t v = fake.rx[fake.rx_head];
    fake.rx_head = (fake.rx_head + 1) % FAKE_RX_WORDS;
    fake.rx_used--;
    fake.rx_reads++;
    if (rx_left > 0 && --rx_left == 0) {
        fake.rx_frames--;
    }
    return v;
}

static void rx_flush(void)
{
    fake.rx_head = fake.rx_used = fake.rx_frames = 0;
    rx_nlen = rx_left = 0;
}

uint32_t ctucan_rd(const ctucan_hw_t *hw, uint32_t off)
{
    (void)hw;
    switch (off) {
    case CTUCAN_DEVICE_ID:
        return fake.device_id;
    case CTUCAN_YOLO:
        return fake.reset_stuck ? 0 : CTUCAN_YOLO_VALUE;
    case CTUCAN_RX_MEM_INFO:
        return FAKE_RX_WORDS | (uint32_t)(FAKE_RX_WORDS - fake.rx_used) << 16;
    case CTUCAN_RX_STATUS:
        return (uint32_t)fake.rx_frames << 4 | (fake.rx_used == 0 ? RXST_RXE : 0);
    case CTUCAN_RX_DATA:
        return rx_pop();
    case CTUCAN_TX_STATUS: {
        uint32_t v = 0;
        for (unsigned b = 0; b < CTUCAN_TXT_BUFFERS; b++) {
            v |= (uint32_t)fake.txt[b] << (4u * b);
        }
        return v;
    }
    case CTUCAN_INT_ENA_SET:
    case CTUCAN_INT_ENA_CLR:
        return fake.int_ena;
    default:
        return fake.mem[off / 4];
    }
}

void ctucan_wr(const ctucan_hw_t *hw, uint32_t off, uint32_t val)
{
    (void)hw;
    if (fake.nlog < FAKE_LOG_MAX) {
        fake.log[fake.nlog].off = off;
        fake.log[fake.nlog].val = val;
        fake.nlog++;
    }
    switch (off) {
    case CTUCAN_MODE:
        if ((val & MODE_RST) && !fake.reset_stuck) {
            fake_reset();
        } else {
            fake.mem[off / 4] = val;
        }
        break;
    case CTUCAN_INT_ENA_SET:
        fake.int_ena |= val;
        break;
    case CTUCAN_INT_ENA_CLR:
        fake.int_ena &= ~val;
        break;
    case CTUCAN_COMMAND:
        if (val & CMD_RRB) {
            rx_flush();
        }
        break;
    case CTUCAN_TX_COMMAND:
        for (unsigned b = 0; b < CTUCAN_TXT_BUFFERS; b++) {
            if (!(val & TXC_TXB(b))) {
                continue;
            }
            if ((val & TXC_TXCR) && ctucan_txt_writable(fake.txt[b])) {
                fake.txt[b] = TXT_RDY;
            } else if ((val & TXC_TXCA) && (fake.txt[b] == TXT_RDY || fake.txt[b] == TXT_TRAN)) {
                fake.txt[b] = TXT_ABT;
            } else if (val & TXC_TXCE) {
                fake.txt[b] = TXT_ETY;
            }
        }
        break;
    default:
        fake.mem[off / 4] = val;
        break;
    }
}

uint32_t fake_last_write(uint32_t off)
{
    for (unsigned i = fake.nlog; i > 0; i--) {
        if (fake.log[i - 1].off == off) {
            return fake.log[i - 1].val;
        }
    }
    return 0;
}

unsigned fake_write_count(uint32_t off)
{
    unsigned n = 0;

    for (unsigned i = 0; i < fake.nlog; i++) {
        n += fake.log[i].off == off;
    }
    return n;
}
