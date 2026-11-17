/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Identify, soft reset, start and stop of src/ctucanfd_hw.c on the fake core:
 * which registers are written, with what, and in which order.
 */
#include <errno.h>
#include "check.h"
#include "fake_core.h"

static ctucan_hw_t hw;

/* Index of the first write to `off` at or after `from`, or fake.nlog. */
static unsigned write_at(uint32_t off, unsigned from)
{
    for (unsigned i = from; i < fake.nlog; i++) {
        if (fake.log[i].off == off) {
            return i;
        }
    }
    return fake.nlog;
}

static void test_identify(void)
{
    unsigned major = 0, minor = 0;

    fake_init(&hw);
    CHECK_EQ(ctucan_hw_identify(&hw, &major, &minor), 0);
    CHECK_EQ(major, 2);
    CHECK_EQ(minor, 2);
    fake.device_id = 0x02020000u | 0xbeefu;
    CHECK_EQ(ctucan_hw_identify(&hw, &major, &minor), ENODEV);
}

static void test_reset(void)
{
    fake_init(&hw);
    fake.mem[CTUCAN_MODE / 4] = SET_ENA | MODE_FDE;
    CHECK_EQ(ctucan_hw_reset(&hw), 0);
    CHECK_EQ(fake.log[0].off, CTUCAN_MODE);
    CHECK_EQ(fake.log[0].val, MODE_RST);
    CHECK_EQ(fake.mem[CTUCAN_MODE / 4] & (MODE_RST | SET_ENA), 0);

    /* a reset that never completes is reported, after a bounded poll */
    fake_init(&hw);
    fake.reset_stuck = true;
    CHECK_EQ(ctucan_hw_reset(&hw), ETIMEDOUT);
}

static void start_with(const ctucan_hw_cfg_t *cfg, uint32_t int_ena)
{
    fake_init(&hw);
    ctucan_hw_start(&hw, cfg, int_ena);
}

static void test_start(void)
{
    const uint32_t ena = INT_RBNEI | INT_DOI | INT_TXBHCI | INT_EWLI | INT_FCSI | INT_BEI;
    ctucan_hw_cfg_t cfg = { .fd = true };

    CHECK_EQ(ctucan_bt_calc(100000000u, 500000u, 875, false, &cfg.nominal), 0);
    CHECK_EQ(ctucan_bt_calc(100000000u, 2000000u, 750, true, &cfg.data), 0);
    start_with(&cfg, ena);

    /* the output is off first and on last, after the core is enabled */
    CHECK_EQ(fake.log[0].off, CTUCAN_INT_ENA_CLR);
    CHECK_EQ(fake.log[0].val, INT_ALL);
    CHECK_EQ(fake.log[fake.nlog - 1].off, CTUCAN_INT_ENA_SET);
    CHECK_EQ(fake.log[fake.nlog - 1].val, ena);
    CHECK_EQ(fake.int_ena, ena);
    CHECK(write_at(CTUCAN_MODE, 0) < fake.nlog - 1, "MODE written before the output is enabled");

    /* sources the driver does not service are masked, the rest unmasked, stale status cleared */
    CHECK_EQ(fake_last_write(CTUCAN_INT_MASK_SET), INT_ALL & ~ena);
    CHECK_EQ(fake_last_write(CTUCAN_INT_MASK_CLR), ena);
    CHECK_EQ(fake_last_write(CTUCAN_INT_STAT), INT_ALL);
    CHECK(write_at(CTUCAN_INT_STAT, 0) < write_at(CTUCAN_MODE, 0), "INT_STAT cleared before the core is enabled");

    /* bit timing written before the core joins the bus */
    CHECK_EQ(fake_last_write(CTUCAN_BTR), ctucan_bt_reg(&cfg.nominal));
    CHECK_EQ(fake_last_write(CTUCAN_BTR_FD), ctucan_bt_reg(&cfg.data));
    CHECK(write_at(CTUCAN_BTR_FD, 0) < write_at(CTUCAN_MODE, 0), "BTR_FD after MODE");
    CHECK_EQ(fake_last_write(CTUCAN_COMMAND), CMD_RRB | CMD_CDO | CMD_RXFCRST | CMD_TXFCRST);

    uint32_t mode = fake_last_write(CTUCAN_MODE);
    CHECK_EQ(mode & (SET_ENA | MODE_FDE), SET_ENA | MODE_FDE);
    CHECK_EQ(mode & (MODE_BMM | SET_ILBP | SET_NISOFD | MODE_RST), 0);

    /* configured with ENA clear, then enabled with the same bits */
    unsigned m1 = write_at(CTUCAN_MODE, 0), m2 = write_at(CTUCAN_MODE, m1 + 1);
    CHECK(m2 < fake.nlog, "MODE written once only");
    CHECK_EQ(fake.log[m1].val & SET_ENA, 0);
    CHECK_EQ(fake.log[m2].val, fake.log[m1].val | SET_ENA);

    /* options map to their MODE and SETTINGS bits */
    cfg.fd = false;
    cfg.listen_only = true;
    cfg.loopback = true;
    cfg.non_iso = true;
    start_with(&cfg, ena);
    mode = fake_last_write(CTUCAN_MODE);
    CHECK_EQ(mode & MODE_FDE, 0);
    CHECK_EQ(mode & (SET_ENA | MODE_BMM | SET_ILBP | SET_NISOFD), SET_ENA | MODE_BMM | SET_ILBP | SET_NISOFD);
}

static void test_stop(void)
{
    ctucan_hw_cfg_t cfg = { .fd = true };

    CHECK_EQ(ctucan_bt_calc(100000000u, 500000u, 875, false, &cfg.nominal), 0);
    CHECK_EQ(ctucan_bt_calc(100000000u, 2000000u, 750, true, &cfg.data), 0);
    start_with(&cfg, INT_RBNEI);
    unsigned first = fake.nlog;
    ctucan_hw_stop(&hw);
    CHECK_EQ(fake.log[first].off, CTUCAN_INT_ENA_CLR);  /* output off before anything else */
    CHECK_EQ(fake.int_ena, 0);
    CHECK_EQ(fake.mem[CTUCAN_MODE / 4] & SET_ENA, 0);
    CHECK_EQ(fake_last_write(CTUCAN_INT_STAT), INT_ALL);
}

int main(void)
{
    test_identify();
    test_reset();
    test_start();
    test_stop();
    return check_exit("test_core");
}
