/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Frame encoding into TXT buffers, validated RX pops from the FIFO, TXT
 * commands and priorities, on the fake core (fake_core.c).
 */
#include <errno.h>
#include <string.h>
#include "check.h"
#include "fake_core.h"

#define STD(id) ((uint32_t)(id) << 18)

static ctucan_hw_t hw;

static uint32_t txtb(unsigned buf, uint32_t word_off)
{
    return fake.mem[(CTUCAN_TXTB(buf) + word_off) / 4];
}

static CAN_MSG msg(uint32_t mid, bool ext, bool rtr, unsigned len)
{
    CAN_MSG m;

    memset(&m, 0, sizeof(m));
    m.mid = mid;
    m.ext.is_extended_mid = ext;
    m.ext.is_remote_frame = rtr;
    m.len = (uint8_t)len;
    for (unsigned i = 0; i < len && i < sizeof(m.dat); i++) {
        m.dat[i] = (uint8_t)(0xa0u + i);
    }
    return m;
}

static unsigned ffw_rwcnt(uint32_t ffw)
{
    return (ffw >> FFW_RWCNT_SHIFT) & FFW_RWCNT_MASK;
}

static void test_tx_classic(void)
{
    fake_init(&hw);
    CAN_MSG m = msg(STD(0x123), false, false, 8);

    CHECK_EQ(ctucan_hw_txt_write(&hw, 0, &m, false, false), 0);
    uint32_t ffw = txtb(0, FRAME_FFW);
    CHECK_EQ(ffw & FFW_DLC_MASK, 8);
    CHECK_EQ(ffw_rwcnt(ffw), 3 + 2);
    CHECK_EQ(ffw & (FFW_FDF | FFW_BRS | FFW_IDE | FFW_RTR), 0);
    CHECK_EQ(txtb(0, FRAME_IDW), STD(0x123));
    CHECK_EQ(txtb(0, FRAME_TSL), 0);
    CHECK_EQ(txtb(0, FRAME_TSU), 0);
    CHECK_EQ(txtb(0, FRAME_DATA), 0xa3a2a1a0u);     /* byte 0 in bits 7:0 */
    CHECK_EQ(txtb(0, FRAME_DATA + 4), 0xa7a6a5a4u);
    CHECK_EQ(fake_write_count(CTUCAN_TX_COMMAND), 0);  /* filling a buffer does not send it */
}

static void test_tx_fd_padding(void)
{
    fake_init(&hw);
    CAN_MSG m = msg(0x1abcdef0u, true, false, 13);

    CHECK_EQ(ctucan_hw_txt_write(&hw, 2, &m, true, true), 0);
    uint32_t ffw = txtb(2, FRAME_FFW);
    CHECK_EQ(ffw & FFW_DLC_MASK, 10);               /* 13 bytes go out as 16 */
    CHECK_EQ(ffw_rwcnt(ffw), 3 + 4);
    CHECK_EQ(ffw & (FFW_FDF | FFW_BRS | FFW_IDE), FFW_FDF | FFW_BRS | FFW_IDE);
    CHECK_EQ(ffw & FFW_RTR, 0);
    CHECK_EQ(txtb(2, FRAME_IDW), 0x1abcdef0u);
    CHECK_EQ(txtb(2, FRAME_DATA + 12), 0x000000acu);    /* byte 12, then zero padding */

    fake_init(&hw);
    m = msg(STD(0x7ff), false, false, 64);
    CHECK_EQ(ctucan_hw_txt_write(&hw, 3, &m, true, false), 0);
    ffw = txtb(3, FRAME_FFW);
    CHECK_EQ(ffw & FFW_DLC_MASK, 15);
    CHECK_EQ(ffw_rwcnt(ffw), 3 + 16);
    CHECK_EQ(ffw & (FFW_FDF | FFW_BRS), FFW_FDF);   /* BRS only when asked for */
    CHECK_EQ(txtb(3, FRAME_DATA + 60), 0xdfdedddcu);
}

static void test_tx_remote(void)
{
    fake_init(&hw);
    CAN_MSG m = msg(STD(0x456), false, true, 4);

    CHECK_EQ(ctucan_hw_txt_write(&hw, 1, &m, false, false), 0);
    uint32_t ffw = txtb(1, FRAME_FFW);
    CHECK_EQ(ffw & FFW_DLC_MASK, 4);                /* a remote frame carries a DLC */
    CHECK_EQ(ffw_rwcnt(ffw), 3);                    /* and no data words */
    CHECK_EQ(ffw & FFW_RTR, FFW_RTR);
    for (unsigned i = 0; i < fake.nlog; i++) {
        CHECK(fake.log[i].off < CTUCAN_TXTB(1) + FRAME_DATA, "data word written for a remote frame");
    }
}

static void test_tx_refused(void)
{
    CAN_MSG m;

    fake_init(&hw);
    m = msg(STD(1), false, true, 4);
    CHECK_EQ(ctucan_hw_txt_write(&hw, 0, &m, true, false), EINVAL);   /* CAN FD has no remote frames */
    m = msg(STD(1), false, false, 9);
    CHECK_EQ(ctucan_hw_txt_write(&hw, 0, &m, false, false), EINVAL);  /* 9 bytes need CAN FD */
    m = msg(STD(1), false, false, 64);
    m.len = 65;
    CHECK_EQ(ctucan_hw_txt_write(&hw, 0, &m, true, false), EINVAL);
    m.len = 8;
    CHECK_EQ(ctucan_hw_txt_write(&hw, CTUCAN_TXT_BUFFERS, &m, false, false), EINVAL);
    CHECK_EQ(fake.nlog, 0);                         /* nothing written for a refused frame */

    /* Datasheet 2.9: only Empty, TX OK, TX failed, Aborted (and parity error) are writable */
    static const unsigned busy[] = { TXT_RDY, TXT_TRAN, TXT_ABTP };
    for (unsigned i = 0; i < 3; i++) {
        fake.txt[0] = busy[i];
        CHECK_EQ(ctucan_hw_txt_write(&hw, 0, &m, false, false), EBUSY);
    }
    static const unsigned done[] = { TXT_ETY, TXT_TOK, TXT_ERR, TXT_ABT, TXT_PER };
    for (unsigned i = 0; i < 5; i++) {
        fake.txt[0] = done[i];
        CHECK_EQ(ctucan_hw_txt_write(&hw, 0, &m, false, false), 0);
    }
}

static void test_txt_cmd_and_prio(void)
{
    fake_init(&hw);
    ctucan_hw_txt_cmd(&hw, TXC_TXCR, 0x5u);
    CHECK_EQ(fake_last_write(CTUCAN_TX_COMMAND), TXC_TXCR | TXC_TXB(0) | TXC_TXB(2));
    CHECK_EQ(ctucan_hw_txt_state(&hw, 0), TXT_RDY);
    CHECK_EQ(ctucan_hw_txt_state(&hw, 1), TXT_ETY);
    CHECK_EQ(ctucan_hw_txt_state(&hw, 2), TXT_RDY);

    /* oldest buffer gets 7, the next ones in ring order 6, 5, 4 */
    ctucan_hw_tx_prio(&hw, 4, 0);
    CHECK_EQ(fake_last_write(CTUCAN_TX_PRIORITY), 0x4567u);
    ctucan_hw_tx_prio(&hw, 4, 2);
    CHECK_EQ(fake_last_write(CTUCAN_TX_PRIORITY), 0x6745u);
    ctucan_hw_tx_prio(&hw, 4, 3);
    CHECK_EQ(fake_last_write(CTUCAN_TX_PRIORITY), 0x7456u);
    ctucan_hw_tx_prio(&hw, 2, 1);
    CHECK_EQ(fake_last_write(CTUCAN_TX_PRIORITY), 0x76u);
    ctucan_hw_tx_prio(&hw, 1, 0);
    CHECK_EQ(fake_last_write(CTUCAN_TX_PRIORITY), 0x7u);
    /* no priority is ever 0: QEMU 6.2 never sends such a buffer */
    for (unsigned n = 1; n <= 4; n++) {
        for (unsigned o = 0; o < n; o++) {
            ctucan_hw_tx_prio(&hw, n, o);
            uint32_t v = fake_last_write(CTUCAN_TX_PRIORITY);
            for (unsigned b = 0; b < n; b++) {
                CHECK(((v >> (4 * b)) & 7u) >= 4, "ntxb %u oldest %u: buffer %u priority 0x%x", n, o, b, v);
            }
        }
    }
}

/* The TX format policy: every combination of -C, -A and -S over every length. */
static void test_tx_flags(void)
{
    for (int fd_mode = 0; fd_mode < 2; fd_mode++) {
        for (int always = 0; always < 2; always++) {
            for (int brs = 0; brs < 2; brs++) {
                for (unsigned len = 0; len <= 64; len++) {
                    ctucan_flags_t f = ctucan_tx_flags(fd_mode, always, brs, len);
                    bool fdf = fd_mode && (always || len > 8);
                    CHECK(f.fdf == fdf, "fd %d -A %d len %u: fdf %d", fd_mode, always, len, f.fdf);
                    CHECK(f.brs == (fdf && brs), "fd %d -A %d brs %d len %u: brs %d", fd_mode, always, brs, len,
                          f.brs);
                    CHECK(!f.esi, "ESI is never set on TX");
                }
            }
        }
    }
    /* the defaults: CAN FD on, BRS on, -A off */
    CHECK(!ctucan_tx_flags(true, false, true, 8).fdf, "8 bytes go out as classic CAN");
    CHECK(ctucan_tx_flags(true, false, true, 9).brs, "9 bytes go out as CAN FD with BRS");
}

/* Builds the FIFO words of one frame: format word, id, two timestamp words, data. */
static unsigned rx_words(uint32_t *w, uint32_t ffw_flags, unsigned dlc, unsigned rwcnt, uint32_t idw,
                         const uint8_t *data, unsigned dwords)
{
    w[0] = ffw_flags | dlc | rwcnt << FFW_RWCNT_SHIFT;
    w[1] = idw;
    w[2] = 0x11111111u;
    w[3] = 0x22222222u;
    for (unsigned i = 0; i < dwords; i++) {
        w[4 + i] = (uint32_t)data[4 * i] | (uint32_t)data[4 * i + 1] << 8 | (uint32_t)data[4 * i + 2] << 16 |
                   (uint32_t)data[4 * i + 3] << 24;
    }
    return 4 + dwords;
}

static void test_rx_frames(void)
{
    uint8_t data[64];
    uint32_t w[20];
    CAN_MSG m;
    ctucan_flags_t fl;

    for (unsigned i = 0; i < 64; i++) {
        data[i] = (uint8_t)(i * 3u);
    }
    fake_init(&hw);
    fake_rx_push(w, rx_words(w, 0, 8, 3 + 2, STD(0x321), data, 2));
    fake_rx_push(w, rx_words(w, FFW_FDF | FFW_BRS | FFW_ESI | FFW_IDE, 15, 3 + 16, 0x12345678u, data, 16));
    CHECK_EQ(ctucan_hw_rx_frames(&hw), 2);

    CHECK_EQ(ctucan_hw_rx(&hw, &m, &fl), 0);
    CHECK_EQ(m.mid, STD(0x321));
    CHECK_EQ(m.ext.is_extended_mid, 0);
    CHECK_EQ(m.ext.is_remote_frame, 0);
    CHECK_EQ(m.len, 8);
    CHECK(memcmp(m.dat, data, 8) == 0, "classic payload");
    CHECK(!fl.fdf && !fl.brs && !fl.esi, "flags of a classic frame");

    CHECK_EQ(ctucan_hw_rx(&hw, &m, &fl), 0);
    CHECK_EQ(m.mid, 0x12345678u);
    CHECK_EQ(m.ext.is_extended_mid, 1);
    CHECK_EQ(m.len, 64);
    CHECK(memcmp(m.dat, data, 64) == 0, "CAN FD payload");
    CHECK(fl.fdf && fl.brs && fl.esi, "FDF, BRS and ESI decoded");
    CHECK_EQ(ctucan_hw_rx_frames(&hw), 0);
    CHECK_EQ(fake.rx_used, 0);

    /* NULL flags pointer is allowed */
    fake_rx_push(w, rx_words(w, 0, 1, 3 + 1, STD(1), data, 1));
    CHECK_EQ(ctucan_hw_rx(&hw, &m, NULL), 0);
}

static void test_rx_remote(void)
{
    uint8_t data[8] = { 0 };
    uint32_t w[20];
    CAN_MSG m;
    ctucan_flags_t fl;

    fake_init(&hw);
    fake_rx_push(w, rx_words(w, FFW_RTR, 6, 3, STD(0x10), data, 0));        /* silicon: no data words */
    fake_rx_push(w, rx_words(w, FFW_RTR, 6, 3 + 2, STD(0x11), data, 2));    /* QEMU 6.2: DLC worth of words */
    fake_rx_push(w, rx_words(w, 0, 0, 3, STD(0x12), data, 0));              /* next frame, must stay intact */
    for (unsigned i = 0; i < 2; i++) {
        CHECK_EQ(ctucan_hw_rx(&hw, &m, &fl), 0);
        CHECK_EQ(m.mid, STD(0x10 + i));
        CHECK_EQ(m.ext.is_remote_frame, 1);
        CHECK_EQ(m.len, 6);
    }
    CHECK_EQ(ctucan_hw_rx(&hw, &m, &fl), 0);
    CHECK_EQ(m.mid, STD(0x12));
    CHECK_EQ(m.len, 0);
}

/* A bad format word must be refused after reading only that word. */
static void test_rx_refused(void)
{
    uint8_t data[64] = { 0 };
    uint32_t w[20];
    CAN_MSG m;
    struct {
        const char *what;
        uint32_t flags;
        unsigned dlc, rwcnt, dwords;
    } bad[] = {
        { "RWCNT one short", FFW_FDF, 15, 3 + 15, 15 },
        { "RWCNT 31", FFW_FDF, 15, 31, 16 },
        { "classic RWCNT too long", 0, 8, 3 + 3, 3 },
        { "remote CAN FD frame", FFW_FDF | FFW_RTR, 4, 3, 0 },
        { "BRS in a classic frame", FFW_BRS, 8, 3 + 2, 2 },
        { "ESI in a classic frame", FFW_ESI, 8, 3 + 2, 2 },
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        fake_init(&hw);
        fake_rx_push(w, rx_words(w, bad[i].flags, bad[i].dlc, bad[i].rwcnt, STD(1), data, bad[i].dwords));
        CHECK(ctucan_hw_rx(&hw, &m, NULL) == EIO, "%s accepted", bad[i].what);
        CHECK(fake.rx_reads == 1, "%s: %u words consumed", bad[i].what, fake.rx_reads);
    }

    /* the format word promises more words than the FIFO holds */
    fake_init(&hw);
    w[0] = FFW_FDF | 15u | (3u + 16u) << FFW_RWCNT_SHIFT;
    fake_rx_push(w, 5);
    CHECK_EQ(ctucan_hw_rx(&hw, &m, NULL), EIO);
    CHECK_EQ(fake.rx_reads, 1);

    /* exactly one word short is refused too */
    fake_init(&hw);
    fake_rx_push(w, rx_words(w, FFW_FDF, 15, 3 + 16, STD(1), data, 16) - 1);
    CHECK_EQ(ctucan_hw_rx(&hw, &m, NULL), EIO);
    CHECK_EQ(fake.rx_reads, 1);

    /* the caller's answer to EIO: flush the whole FIFO */
    ctucan_hw_rx_flush(&hw);
    CHECK_EQ(fake_last_write(CTUCAN_COMMAND), CMD_RRB);
    CHECK_EQ(ctucan_hw_rx_frames(&hw), 0);
}

/* Whatever the TX side writes into a TXT buffer, the RX side reads back as
 * the same frame: every length, both id formats, classic and CAN FD. */
static void test_round_trip(void)
{
    unsigned n = 0;

    for (int ext = 0; ext < 2; ext++) {
        for (unsigned len = 0; len <= 64; len++) {
            for (int fd = len > 8; fd < 2; fd++) {
                for (int brs = 0; brs <= fd; brs++) {
                    CAN_MSG in = msg(ext ? 0x0abcdef1u : STD(0x5a5), ext, false, len), out;
                    ctucan_flags_t fl;

                    fake_init(&hw);
                    CHECK_EQ(ctucan_hw_txt_write(&hw, 1, &in, fd, brs), 0);
                    uint32_t w[20];
                    unsigned words = 1 + ffw_rwcnt(txtb(1, FRAME_FFW));
                    for (unsigned i = 0; i < words; i++) {
                        w[i] = txtb(1, 4 * i);
                    }
                    fake_rx_push(w, words);
                    CHECK(ctucan_hw_rx(&hw, &out, &fl) == 0, "len %u fd %d: refused", len, fd);
                    unsigned want = fd ? ctucan_dlc2len(ctucan_len2dlc(len), true) : len;
                    CHECK(out.len == want, "len %u fd %d: got %u", len, fd, out.len);
                    CHECK_EQ(out.mid, in.mid);
                    CHECK_EQ(out.ext.is_extended_mid, (unsigned)ext);
                    CHECK(memcmp(out.dat, in.dat, len) == 0, "len %u fd %d: payload", len, fd);
                    for (unsigned i = len; i < want; i++) {
                        CHECK(out.dat[i] == 0, "len %u: padding byte %u is 0x%02x", len, i, out.dat[i]);
                    }
                    CHECK(fl.fdf == (bool)fd && fl.brs == (bool)brs && !fl.esi, "len %u: flags", len);
                    CHECK_EQ(fake.rx_used, 0);
                    n++;
                }
            }
        }
    }
    CHECK(n == 278, "%u round trips, want 278", n);   /* 2 id formats x (9 x 3 + 56 x 2) */
}

int main(void)
{
    test_tx_classic();
    test_tx_fd_padding();
    test_tx_remote();
    test_tx_refused();
    test_txt_cmd_and_prio();
    test_tx_flags();
    test_rx_frames();
    test_rx_remote();
    test_rx_refused();
    test_round_trip();
    return check_exit("test_frames");
}
