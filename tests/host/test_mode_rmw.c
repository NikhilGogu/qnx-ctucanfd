/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * MODE and SETTINGS are one word, and some of its bits are not the driver's:
 * MODE[RXBAM] and SETTINGS[TBFBO] reset to 1 (datasheet 3.1.3, 3.1.4). The
 * fake core here starts at the datasheet reset value, so that start, stop and
 * a soft reset followed by start must keep both. QEMU 6.2 ignores both bits,
 * which is why no other test sees a write that clears them.
 */
#include "check.h"
#include "fake_core.h"

static ctucan_hw_t hw;

#define KEEP_RESET      (MODE_RXBAM | SET_TBFBO)
#define DS_RESET        (MODE_FDE | KEEP_RESET)   /* datasheet reset value of the bits that are 1 */
#define ERFM            (1u << 12)                /* MODE[ERFM], reset X (build parameter) */
#define PCHKE           (1u << 27)                /* SETTINGS[PCHKE], reset X (build parameter) */

static void cfg_default(ctucan_hw_cfg_t *cfg)
{
    *cfg = (ctucan_hw_cfg_t){ .fd = true };
    CHECK_EQ(ctucan_bt_calc(100000000u, 500000u, 875, false, &cfg->nominal), 0);
    CHECK_EQ(ctucan_bt_calc(100000000u, 2000000u, 750, true, &cfg->data), 0);
}

/* Every write of MODE keeps the bits that are not the driver's. */
static void check_all_mode_writes(uint32_t keep, const char *what)
{
    unsigned n = 0;

    for (unsigned i = 0; i < fake.nlog; i++) {
        if (fake.log[i].off == CTUCAN_MODE) {
            n++;
            CHECK((fake.log[i].val & keep) == keep, "%s: MODE write %u is 0x%08x, lost 0x%08x", what, n,
                  fake.log[i].val, keep & ~fake.log[i].val);
        }
    }
    CHECK(n > 0, "%s: no MODE write seen", what);
}

static void test_start_keeps_reset_bits(void)
{
    ctucan_hw_cfg_t cfg;

    cfg_default(&cfg);
    fake_init(&hw);
    fake.mode_reset = DS_RESET;
    fake.mem[CTUCAN_MODE / 4] = DS_RESET;
    ctucan_hw_start(&hw, &cfg, INT_RBNEI);
    CHECK_EQ(fake.mem[CTUCAN_MODE / 4] & MODE_RXBAM, MODE_RXBAM);     /* RX buffer automatic mode */
    CHECK_EQ(fake.mem[CTUCAN_MODE / 4] & SET_TBFBO, SET_TBFBO);       /* buffers fail at bus-off */
    CHECK_EQ(fake.mem[CTUCAN_MODE / 4] & (MODE_FDE | SET_ENA), MODE_FDE | SET_ENA);
    check_all_mode_writes(KEEP_RESET, "start");
}

/* Bits that the build parameters decide (reset X) are kept as the core has them. */
static void test_start_keeps_other_bits(void)
{
    ctucan_hw_cfg_t cfg;

    cfg_default(&cfg);
    fake_init(&hw);
    fake.mem[CTUCAN_MODE / 4] = DS_RESET | ERFM | PCHKE | MODE_ACF | SET_PEX;
    ctucan_hw_start(&hw, &cfg, INT_RBNEI);
    CHECK_EQ(fake.mem[CTUCAN_MODE / 4] & (KEEP_RESET | ERFM | PCHKE | MODE_ACF | SET_PEX),
             KEEP_RESET | ERFM | PCHKE | MODE_ACF | SET_PEX);
    check_all_mode_writes(KEEP_RESET | ERFM | PCHKE | MODE_ACF | SET_PEX, "start, other bits");
}

/* The driver's own bits follow the configuration, whatever the core held before. */
static void test_owned_bits_follow_cfg(void)
{
    ctucan_hw_cfg_t cfg;
    const uint32_t stale = MODE_FDE | MODE_BMM | SET_ILBP | SET_NISOFD | SET_RTRLE |
                           (SET_RTRTH_MAX << SET_RTRTH_SHIFT) | SET_ENA;

    cfg_default(&cfg);
    cfg.fd = false;     /* everything the core held is switched off */
    fake_init(&hw);
    fake.mem[CTUCAN_MODE / 4] = DS_RESET | stale;
    ctucan_hw_start(&hw, &cfg, INT_RBNEI);
    CHECK_EQ(fake.mem[CTUCAN_MODE / 4] & (CTUCAN_MODE_OWNED & ~SET_ENA), 0);
    CHECK_EQ(fake.mem[CTUCAN_MODE / 4] & SET_ENA, SET_ENA);
    CHECK_EQ(fake.mem[CTUCAN_MODE / 4] & KEEP_RESET, KEEP_RESET);
    /* ENA only in the second write: the configuration lands first */
    unsigned m1 = fake.nlog, m2 = fake.nlog;
    for (unsigned i = 0; i < fake.nlog; i++) {
        if (fake.log[i].off == CTUCAN_MODE) {
            if (m1 == fake.nlog) {
                m1 = i;
            } else if (m2 == fake.nlog) {
                m2 = i;
            }
        }
    }
    CHECK(m2 < fake.nlog, "two MODE writes");
    CHECK_EQ(fake.log[m1].val & SET_ENA, 0);
    CHECK_EQ(fake.log[m2].val, fake.log[m1].val | SET_ENA);

    cfg.fd = true;
    cfg.listen_only = cfg.loopback = cfg.non_iso = true;
    cfg.retr_limit = true;
    cfg.retr_max = 5;
    fake_init(&hw);
    fake.mem[CTUCAN_MODE / 4] = DS_RESET & ~MODE_FDE;
    ctucan_hw_start(&hw, &cfg, INT_RBNEI);
    uint32_t want = MODE_FDE | MODE_BMM | SET_ILBP | SET_NISOFD | SET_RTRLE | (5u << SET_RTRTH_SHIFT) | SET_ENA;
    CHECK_EQ(fake.mem[CTUCAN_MODE / 4], want | KEEP_RESET);
}

static void test_stop_keeps_reset_bits(void)
{
    ctucan_hw_cfg_t cfg;

    cfg_default(&cfg);
    fake_init(&hw);
    fake.mem[CTUCAN_MODE / 4] = DS_RESET;
    ctucan_hw_start(&hw, &cfg, INT_RBNEI);
    unsigned first = fake.nlog;
    ctucan_hw_stop(&hw);
    CHECK_EQ(fake.mem[CTUCAN_MODE / 4] & SET_ENA, 0);
    CHECK_EQ(fake.mem[CTUCAN_MODE / 4] & KEEP_RESET, KEEP_RESET);
    for (unsigned i = first; i < fake.nlog; i++) {
        if (fake.log[i].off == CTUCAN_MODE) {
            CHECK_EQ(fake.log[i].val & KEEP_RESET, KEEP_RESET);
        }
    }
}

/* The restart path of the driver: stop, soft reset to the datasheet values, start. */
static void test_restart_sequence(void)
{
    ctucan_hw_cfg_t cfg;

    cfg_default(&cfg);
    fake_init(&hw);
    fake.mode_reset = DS_RESET;
    CHECK_EQ(ctucan_hw_reset(&hw), 0);
    CHECK_EQ(fake.mem[CTUCAN_MODE / 4], DS_RESET);
    ctucan_hw_start(&hw, &cfg, INT_RBNEI);
    ctucan_hw_stop(&hw);
    CHECK_EQ(ctucan_hw_reset(&hw), 0);
    ctucan_hw_start(&hw, &cfg, INT_RBNEI);
    CHECK_EQ(fake.mem[CTUCAN_MODE / 4], DS_RESET | SET_ENA);
}

int main(void)
{
    test_start_keeps_reset_bits();
    test_start_keeps_other_bits();
    test_owned_bits_follow_cfg();
    test_stop_keeps_reset_bits();
    test_restart_sequence();
    return check_exit("test_mode_rmw");
}
