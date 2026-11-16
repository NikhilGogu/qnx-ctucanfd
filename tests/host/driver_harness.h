/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared by the driver tests: includes src/dev-can-ctucanfd.c whole, with
 * its main() renamed, and gives the helpers that drive it through the fake
 * libcan and the fake core. Include it in exactly one file per test binary.
 */
#ifndef CTUCAN_HOST_DRIVER_HARNESS_H
#define CTUCAN_HOST_DRIVER_HARNESS_H

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "check.h"
#include "fake_core.h"
#include "fake_libcan.h"

#define main drv_main
#include "dev-can-ctucanfd.c"
#undef main

enum { RX0 = 0, TX1 = 1 };      /* dev_unit with the default -r 1 -t 1 */

static inline int run(const char *args, void (*body)(void))
{
    memset(&g_drv, 0, sizeof(g_drv));
#if defined(__GLIBC__)
    optind = 0;                 /* glibc: start getopt() afresh */
#else
    optreset = 1;
    optind = 1;
#endif
    int rc = fake_run_driver(drv_main, args, body);
#ifdef CTUCAN_FI
    free(g_drv.hw.fi);      /* the driver keeps it until the process ends */
    g_drv.hw.fi = NULL;
#endif
    return rc;
}

static inline CAN_MSG frame(uint32_t id, unsigned len)
{
    CAN_MSG m;

    memset(&m, 0, sizeof(m));
    m.mid = (id & 0x7ffu) << 18;
    m.len = (uint8_t)len;
    for (unsigned i = 0; i < len; i++) {
        m.dat[i] = (uint8_t)(id + i);
    }
    return m;
}

static inline void irq(uint32_t bits)
{
    fake.mem[CTUCAN_INT_STAT / 4] |= bits;
}

/* A classic frame with an 11-bit id arrives in the RX FIFO. */
static inline void rx_frame(uint32_t id, unsigned len)
{
    uint32_t w[4 + 2] = { 0 };
    unsigned dw = (len + 3u) / 4u;

    w[0] = len | ((3u + dw) << FFW_RWCNT_SHIFT);
    w[1] = (id & 0x7ffu) << 18;
    for (unsigned i = 0; i < len; i++) {
        w[4 + i / 4] |= (uint32_t)(uint8_t)(id + i) << (8u * (i % 4u));
    }
    fake_rx_push(w, 4 + dw);
}

static inline ctucan_xstats_t xstats(void)
{
    ctucan_xstats_t x;

    memset(&x, 0, sizeof(x));
    CHECK_EQ(fake_devctl(RX0, CTUCAN_DEVCTL_GET_XSTATS, &x, 0, sizeof(x)), sizeof(x));
    return x;
}

static inline CAN_DEVCTL_STATS stats(void)
{
    CAN_DEVCTL_STATS s;

    memset(&s, 0, sizeof(s));
    CHECK_EQ(fake_devctl(RX0, CAN_DEVCTL_GET_STATS, &s, 0, sizeof(s)), sizeof(s));
    return s;
}

static inline uint32_t txt_id(unsigned b)
{
    return fake.mem[(CTUCAN_TXTB(b) + FRAME_IDW) / 4] >> 18;
}

static inline unsigned ercrst_writes(void)
{
    unsigned n = 0;

    for (unsigned i = 0; i < fake.nlog; i++) {
        n += fake.log[i].off == CTUCAN_COMMAND && (fake.log[i].val & CMD_ERCRST) != 0;
    }
    return n;
}

static inline void set_fault(uint32_t fs, unsigned tec)
{
    fake.mem[CTUCAN_EWL_ERP_FAULT / 4] = fs | 96u;
    fake.mem[CTUCAN_REC_TEC / 4] = (uint32_t)tec << 16;
    irq(INT_FCSI);
}

static inline bool busoff_hold(void)
{
    return (xstats().state >> 9) & 1u;
}

static inline int dc(unsigned dev, int dcmd, void *data, size_t in, size_t out)
{
    errno = 0;
    return fake_devctl(dev, dcmd, data, in, out);
}

#endif
