/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * No sleep on libcan's thread: a SET_TIMING restart and a bus-off, each with
 * 4 frames whose aborts never finish, handled with no nanosleep() inside the
 * driver's libcan callbacks (event handler, transmit, devctl). The fake
 * libcan counts every nanosleep() made while one of them runs.
 *
 * It drives the driver only through what any version of it gives libcan, the
 * standard SET_TIMING devctl, GET_XSTATS fields that every version has and
 * the fake core, so it also builds against an older driver source:
 *   make -C tests/host no_sleep_base BASE=/path/to/other/src
 * The sleep-poll driver (30fff05) fails it; the shutdown wait, which runs
 * after can_resmgr_start() has returned, is not counted.
 */
#include "driver_harness.h"

static const CAN_DEVCTL_TIMING T250 = { .bit_rate_prescaler = 8, .time_segment_1 = 43, .time_segment_2 = 6,
                                        .sync_jump_width = 6 };

/* What a case left behind, read before the body returns (shutdown follows). */
static ctucan_xstats_t g_x0, g_x1;
static unsigned g_sleeps;
static uint64_t g_slept_ns;

static void stuck_frames(void)
{
    for (unsigned i = 0; i < 4; i++) {
        CAN_MSG m = frame(0x600 + i, 8);
        CHECK_EQ(fake_write(TX1, &m), 0);
    }
    fake.abort_slow = true;     /* the core never finishes them, nor their aborts */
    g_x0 = xstats();
}

/* Whatever the driver needs from outside to end its wait: the wait timer's
 * devctl when it has one, then a pulse. A driver that waits inside the
 * callback is done already. */
static void finish(void)
{
    for (unsigned k = 0; k < 8 && fake_timer_run(); k++) {
    }
    fake_pulse();
    (void)stats();              /* one more client request after the fault */
    g_x1 = xstats();
    g_sleeps = lc.cb_sleeps;
    g_slept_ns = lc.cb_slept_ns;
}

static void body_set_timing(void)
{
    CAN_DEVCTL_TIMING t = T250;

    stuck_frames();
    CHECK_EQ(dc(RX0, CAN_DEVCTL_SET_TIMING, &t, sizeof(t), 0), 0);
    (void)stats();              /* a client asks while the restart is handled */
    finish();
}

static void body_busoff(void)
{
    stuck_frames();
    set_fault(FS_BOF, 256);
    fake_pulse();
    (void)stats();
    finish();
    set_fault(FS_ERA, 0);       /* the bus comes back */
    fake_pulse();
    g_sleeps = lc.cb_sleeps;
    g_slept_ns = lc.cb_slept_ns;
}

static void one(const char *name, const char *args, void (*body)(void), unsigned restarts)
{
    g_sleeps = 0;
    g_slept_ns = 0;
    memset(&g_x0, 0, sizeof(g_x0));
    memset(&g_x1, 0, sizeof(g_x1));
    CHECK_EQ(run(args, body), 0);
    printf("%s: 4 stuck frames, %u given up, %u restart(s); %u nanosleep() call(s) inside libcan callbacks, "
           "%.1f ms\n", name, g_x1.tx_abort_forced - g_x0.tx_abort_forced, g_x1.restarts - g_x0.restarts, g_sleeps,
           (double)g_slept_ns / 1e6);
    /* the case did what it claims, on any driver */
    CHECK(g_x0.state % 256u == 4, "%s: %u frames in flight before the fault, not 4", name, g_x0.state % 256u);
    CHECK(g_x1.state % 256u == 0, "%s: %u frames still in flight", name, g_x1.state % 256u);
    CHECK(g_x1.tx_abort_forced - g_x0.tx_abort_forced == 4, "%s: %u frames given up, not 4", name,
          g_x1.tx_abort_forced - g_x0.tx_abort_forced);
    CHECK(g_x1.restarts - g_x0.restarts == restarts, "%s: %u restarts, not %u", name, g_x1.restarts - g_x0.restarts,
          restarts);
    /* the property */
    CHECK(g_sleeps == 0, "%s: libcan's thread slept %u time(s), %.1f ms in all", name, g_sleeps,
          (double)g_slept_ns / 1e6);
}

int main(void)
{
    one("SET_TIMING restart", "", body_set_timing, 1);
    one("bus-off", "", body_busoff, 0);
    one("bus-off with -a", "-a", body_busoff, 0);
    return check_exit("test_no_sleep");
}
