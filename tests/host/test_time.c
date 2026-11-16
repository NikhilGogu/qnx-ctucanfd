/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Timestamp arithmetic (src/ctucanfd_time.h): wrap, offsets set before and
 * after a wrap, and agreement of the 32-bit and 64-bit values.
 */
#include "check.h"
#include "ctucanfd_time.h"

#define W32 0x100000000ull

static const uint64_t monos[] = { 0, 1, 999999, 0xfffffffeull, 0xffffffffull, W32, W32 + 1, 5 * W32 - 7,
                                  1ull << 40, (1ull << 52) + 12345 };
static const uint32_t wants[] = { 0, 1, 16199300u, 0x7fffffffu, 0x80000000u, 0xffffff00u, 0xffffffffu };

static void test_wrap(void)
{
    CHECK_EQ(ctucan_ts32(0xffffffffull, 0), 0xffffffffu);
    CHECK_EQ(ctucan_ts32(W32, 0), 0);               /* wraps at 2^32 us */
    CHECK_EQ(ctucan_ts32(W32 + 5, 0), 5);
    CHECK_EQ(ctucan_ts32(10, 0xfffffff0u), 0xfffffffau);
    CHECK_EQ(ctucan_ts32(20, 0xfffffff0u), 4);      /* the offset wraps it too */
}

static void test_set_then_read(void)
{
    static const uint64_t later[] = { 0, 1, 255, 256, 1000000, 0xffffffffull, W32, 3 * W32 + 17 };

    for (size_t i = 0; i < sizeof(monos) / sizeof(monos[0]); i++) {
        for (size_t j = 0; j < sizeof(wants) / sizeof(wants[0]); j++) {
            uint32_t off = ctucan_ts_offset_for(monos[i], wants[j]);
            CHECK(ctucan_ts32(monos[i], off) == wants[j], "set %u at %llu", wants[j],
                  (unsigned long long)monos[i]);
            for (size_t k = 0; k < sizeof(later) / sizeof(later[0]); k++) {
                uint64_t m = monos[i] + later[k];
                CHECK(ctucan_ts32(m, off) == (uint32_t)(wants[j] + later[k]), "set %u, %llu us later",
                      wants[j], (unsigned long long)later[k]);
                CHECK(ctucan_ts64(m, off) - ctucan_ts64(monos[i], off) == later[k], "64-bit elapsed time");
                CHECK((uint32_t)ctucan_ts64(m, off) == ctucan_ts32(m, off), "low 32 bits of the 64-bit value");
            }
        }
    }
}

/* Setting the same value twice gives the same offset, whatever came before:
 * the offset depends only on the time of the set and the value wanted. */
static void test_set_is_absolute(void)
{
    uint32_t a = ctucan_ts_offset_for(W32 + 100, 42);
    uint32_t b = ctucan_ts_offset_for(W32 + 100, 42);

    CHECK_EQ(a, b);
    CHECK_EQ(ctucan_ts_offset_for(100, 100), 0);
}

/* The 64-bit value keeps counting where the 32-bit one wraps. */
static void test_64_monotonic(void)
{
    uint32_t off = ctucan_ts_offset_for(1000, 0xffffff00u);
    uint64_t prev = ctucan_ts64(1000, off);

    for (uint64_t m = 1001; m < 1000 + 0x200; m++) {
        uint64_t v = ctucan_ts64(m, off);
        CHECK(v == prev + 1, "64-bit timestamp not monotonic at %llu", (unsigned long long)m);
        prev = v;
    }
    CHECK(ctucan_ts32(1000 + 0x1ff, off) < 0x100u, "the 32-bit value wrapped");
}

int main(void)
{
    test_wrap();
    test_set_then_read();
    test_set_is_absolute();
    test_64_monotonic();
    return check_exit("test_time");
}
