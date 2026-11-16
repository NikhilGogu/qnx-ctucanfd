/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * DLC tables and bit timing of src/ctucanfd_hw.c.
 */
#include <errno.h>
#include <stdlib.h>
#include "check.h"
#include "ctucanfd_hw.h"

static const unsigned fd_sizes[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64 };

static void test_dlc(void)
{
    for (unsigned dlc = 0; dlc < 16; dlc++) {
        CHECK_EQ(ctucan_dlc2len(dlc, true), fd_sizes[dlc]);
        CHECK_EQ(ctucan_dlc2len(dlc, false), dlc > 8 ? 8u : dlc);  /* classic DLC 9..15 means 8 bytes */
    }
    CHECK_EQ(ctucan_dlc2len(0x1f, true), 64);      /* only the 4 DLC bits count */

    /* len2dlc picks the smallest CAN FD size that holds the payload */
    for (unsigned len = 0; len <= 64; len++) {
        unsigned dlc = ctucan_len2dlc(len);
        unsigned want = 0;
        while (fd_sizes[want] < len) {
            want++;
        }
        CHECK(dlc == want, "len %u -> dlc %u, want %u", len, dlc, want);
        CHECK(ctucan_dlc2len(dlc, true) >= len, "len %u does not fit dlc %u", len, dlc);
    }
}

/* The defaults the driver prints at start-up (README): 500 kbit/s at 87.5 %
 * and 2 Mbit/s at 75 % from a 100 MHz clock, both 50 time quanta. */
static void test_bt_defaults(void)
{
    ctucan_bt_t n, d;

    CHECK_EQ(ctucan_bt_calc(100000000u, 500000u, 875, false, &n), 0);
    CHECK_EQ(n.brp, 4);
    CHECK_EQ(1u + n.prop + n.ph1 + n.ph2, 50);
    CHECK_EQ(n.prop + n.ph1, 43);       /* time_segment_1 that canctl shows */
    CHECK_EQ(n.ph2, 6);
    CHECK_EQ(n.sjw, 6);

    CHECK_EQ(ctucan_bt_calc(100000000u, 2000000u, 750, true, &d), 0);
    CHECK_EQ(d.brp, 1);
    CHECK_EQ(1u + d.prop + d.ph1 + d.ph2, 50);
    CHECK(d.ph1 <= 31 && d.ph2 <= 31 && d.prop <= 63, "data phase fields out of range");
}

/* Every result must give the bit rate exactly, pass ctucan_bt_check() and put
 * the sample point within half a time quantum of the one asked for. */
static void test_bt_properties(void)
{
    static const uint32_t clocks[] = { 20000000u, 40000000u, 80000000u, 100000000u };
    static const uint32_t nominal[] = { 10000u, 20000u, 50000u, 100000u, 125000u, 250000u, 500000u, 800000u,
                                        1000000u };
    static const uint32_t data[] = { 1000000u, 2000000u, 4000000u, 5000000u, 8000000u, 10000000u };
    static const unsigned sps[] = { 500, 700, 750, 800, 875, 900, 950 };
    unsigned found = 0;

    for (size_t c = 0; c < sizeof(clocks) / sizeof(clocks[0]); c++) {
        for (int phase = 0; phase < 2; phase++) {
            const uint32_t *rates = phase ? data : nominal;
            size_t nrates = phase ? sizeof(data) / sizeof(data[0]) : sizeof(nominal) / sizeof(nominal[0]);
            for (size_t r = 0; r < nrates; r++) {
                for (size_t s = 0; s < sizeof(sps) / sizeof(sps[0]); s++) {
                    ctucan_bt_t bt;
                    if (ctucan_bt_calc(clocks[c], rates[r], sps[s], phase, &bt) != 0) {
                        continue;
                    }
                    found++;
                    unsigned ntq = 1u + bt.prop + bt.ph1 + bt.ph2;
                    CHECK((uint64_t)bt.brp * ntq * rates[r] == clocks[c], "clk %u rate %u: brp %u ntq %u",
                          clocks[c], rates[r], bt.brp, ntq);
                    CHECK_EQ(ctucan_bt_check(&bt, phase), 0);
                    long sp = (long)(1000u * (1u + bt.prop + bt.ph1)) / (long)ntq;
                    long tol = 500 / (long)ntq + 1;
                    CHECK(labs(sp - (long)sps[s]) <= tol, "clk %u rate %u: sample point %ld for %u", clocks[c],
                          rates[r], sp, sps[s]);
                }
            }
        }
    }
    CHECK(found > 100, "only %u timings found", found);
}

static void test_bt_refused(void)
{
    ctucan_bt_t bt;

    CHECK_EQ(ctucan_bt_calc(0, 500000u, 875, false, &bt), EINVAL);
    CHECK_EQ(ctucan_bt_calc(100000000u, 0, 875, false, &bt), EINVAL);
    CHECK_EQ(ctucan_bt_calc(100000000u, 500000u, 499, false, &bt), EINVAL);
    CHECK_EQ(ctucan_bt_calc(100000000u, 500000u, 951, false, &bt), EINVAL);
    /* 100 MHz is no whole multiple of 8 Mbit/s times any prescaler */
    CHECK_EQ(ctucan_bt_calc(100000000u, 8000000u, 750, true, &bt), EINVAL);
    /* fewer than 8 time quanta per bit */
    CHECK_EQ(ctucan_bt_calc(20000000u, 5000000u, 750, true, &bt), EINVAL);
}

static void test_bt_check(void)
{
    const ctucan_bt_t ok = { .brp = 4, .prop = 22, .ph1 = 21, .ph2 = 6, .sjw = 6 };
    ctucan_bt_t b;

    CHECK_EQ(ctucan_bt_check(&ok, false), 0);
    b = ok; b.sjw = 7;  CHECK_EQ(ctucan_bt_check(&b, false), EINVAL);    /* larger than ph2 */
    b = ok; b.sjw = 0;  CHECK_EQ(ctucan_bt_check(&b, false), EINVAL);
    b = ok; b.brp = 0;  CHECK_EQ(ctucan_bt_check(&b, false), EINVAL);
    b = ok; b.brp = 256; CHECK_EQ(ctucan_bt_check(&b, false), EINVAL);
    b = ok; b.ph2 = 0;  CHECK_EQ(ctucan_bt_check(&b, false), EINVAL);
    b = ok; b.prop = 127; CHECK_EQ(ctucan_bt_check(&b, false), 0);
    b = ok; b.prop = 128; CHECK_EQ(ctucan_bt_check(&b, false), EINVAL);
    b = ok; b.prop = 64; CHECK_EQ(ctucan_bt_check(&b, true), EINVAL);     /* data phase PROP is 6 bits */
    b = ok; b.ph1 = 32; CHECK_EQ(ctucan_bt_check(&b, true), EINVAL);      /* data phase PH1 is 5 bits */
    b = (ctucan_bt_t){ .brp = 1, .prop = 1, .ph1 = 1, .ph2 = 1, .sjw = 1 };
    CHECK_EQ(ctucan_bt_check(&b, false), EINVAL);                         /* 4 quanta, fewer than 8 */
}

/* BTR: PROP [6:0], PH1 [12:7], PH2 [18:13], BRP [26:19], SJW [31:27]. */
static void test_bt_reg(void)
{
    const ctucan_bt_t bt = { .brp = 4, .prop = 22, .ph1 = 21, .ph2 = 6, .sjw = 6 };
    const ctucan_bt_t max = { .brp = 255, .prop = 127, .ph1 = 63, .ph2 = 63, .sjw = 31 };
    uint32_t v = ctucan_bt_reg(&bt), m = ctucan_bt_reg(&max);

    CHECK_EQ(v, 22u | 21u << 7 | 6u << 13 | 4u << 19 | 6u << 27);
    CHECK_EQ(m & 0x7fu, 127);
    CHECK_EQ((m >> 7) & 0x3fu, 63);
    CHECK_EQ((m >> 13) & 0x3fu, 63);
    CHECK_EQ((m >> 19) & 0xffu, 255);
    CHECK_EQ(m >> 27, 31);
    CHECK_EQ(m, 0xffffffffu);   /* the five fields tile the word with no overlap */
}

int main(void)
{
    test_dlc();
    test_bt_defaults();
    test_bt_properties();
    test_bt_refused();
    test_bt_check();
    test_bt_reg();
    return check_exit("test_dlc_timing");
}
