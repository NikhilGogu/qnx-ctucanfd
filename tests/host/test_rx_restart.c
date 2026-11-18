/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * No RX frame lost to a restart: frames that are in the RX FIFO when the
 * controller is reset reach the readers. Cases: a SET_TIMING restart with
 * nothing in flight, a SET_TIMING restart deferred behind 4 frames whose
 * aborts never finish, and a bus-off with -a and the same 4 frames (recovery
 * is COMMAND.ERCRST, not a reset; the case guards that the FIFO survives it).
 * In each case 10 frames are in the FIFO before the request and no interrupt
 * pulse runs until the restart is over; then pulses run, as the pending
 * RBNEI brings them, and 10 blocked readers take what is queued.
 *
 * It drives the driver only through what any version of it gives libcan, the
 * standard SET_TIMING devctl, GET_STATS, GET_XSTATS fields that every version
 * has and the fake core, so it also builds against an older driver source:
 *   make -C tests/host rx_restart_base BASE=/path/to/other/src
 * A driver whose reset empties the FIFO unread fails the SET_TIMING cases.
 */
#include "driver_harness.h"

#define NRX 10u

static const CAN_DEVCTL_TIMING T250 = { .bit_rate_prescaler = 8, .time_segment_1 = 43, .time_segment_2 = 6,
                                        .sync_jump_width = 6 };

/* What a case left behind, read before the body returns (shutdown follows). */
static ctucan_xstats_t g_x0, g_x1;
static uint32_t g_rx0, g_rx1;
static unsigned g_got, g_in_order;

static void fifo_frames(void)
{
    for (unsigned i = 0; i < NRX; i++) {
        rx_frame(0x300 + i, 8);
    }
    fake_block_readers(RX0, NRX);
}

static void stuck_frames(void)
{
    for (unsigned i = 0; i < 4; i++) {
        CAN_MSG m = frame(0x600 + i, 8);
        CHECK_EQ(fake_write(TX1, &m), 0);
    }
    fake.abort_slow = true;     /* the core never finishes them, nor their aborts */
}

static void begin(void)
{
    g_x0 = xstats();
    g_rx0 = stats().received_frames;
    lc.nrx = 0;
}

/* Pulses until the readers stop getting frames, then what they got. */
static void end(void)
{
    for (unsigned k = 0; k < 8; k++) {
        unsigned before = lc.nrx;
        fake_pulse();
        if (k > 0 && lc.nrx == before) {
            break;
        }
    }
    g_x1 = xstats();
    g_rx1 = stats().received_frames;
    g_got = lc.nrx;
    g_in_order = 0;
    for (unsigned i = 0; i < lc.nrx && i < NRX; i++) {
        g_in_order += lc.rx[i].msg.mid == (0x300u + i) << 18;
    }
}

static void body_plain(void)
{
    CAN_DEVCTL_TIMING t = T250;

    fifo_frames();
    begin();
    CHECK_EQ(dc(RX0, CAN_DEVCTL_SET_TIMING, &t, sizeof(t), 0), 0);
    end();
}

static void body_deferred(void)
{
    CAN_DEVCTL_TIMING t = T250;

    stuck_frames();
    fifo_frames();
    begin();
    CHECK_EQ(dc(RX0, CAN_DEVCTL_SET_TIMING, &t, sizeof(t), 0), 0);
    for (unsigned k = 0; k < 8 && fake_timer_run(); k++) {
    }
    end();
}

static void body_busoff(void)
{
    stuck_frames();
    set_fault(FS_BOF, 256);
    fake_pulse();               /* bus-off is seen; the FIFO is empty yet */
    fifo_frames();
    begin();
    for (unsigned k = 0; k < 8 && fake_timer_run(); k++) {
    }
    set_fault(FS_ERA, 0);       /* the bus comes back */
    end();
}

static void one(const char *name, const char *args, void (*body)(void), unsigned restarts)
{
    g_got = g_in_order = 0;
    g_rx0 = g_rx1 = 0;
    memset(&g_x0, 0, sizeof(g_x0));
    memset(&g_x1, 0, sizeof(g_x1));
    CHECK_EQ(run(args, body), 0);
    unsigned flushed = g_x1.rx_flushed - g_x0.rx_flushed;
    printf("%s: %u frames in the RX FIFO, %u restart(s); %u received, %u flushed by the reset, %u reached readers "
           "(%u in order)\n", name, NRX, g_x1.restarts - g_x0.restarts, g_rx1 - g_rx0, flushed, g_got, g_in_order);
    /* the case did what it claims, on any driver */
    CHECK(g_x1.restarts - g_x0.restarts == restarts, "%s: %u restarts, not %u", name, g_x1.restarts - g_x0.restarts,
          restarts);
    CHECK(g_x1.restart_fail == g_x0.restart_fail, "%s: a restart failed", name);
    /* the property */
    CHECK(flushed == 0, "%s: the reset flushed %u frame(s) from the RX FIFO", name, flushed);
    CHECK(g_rx1 - g_rx0 == NRX, "%s: %u of %u frames received", name, g_rx1 - g_rx0, NRX);
    CHECK(g_got == NRX && g_in_order == NRX, "%s: %u of %u frames reached readers, %u in order", name, g_got, NRX,
          g_in_order);
}

int main(void)
{
    one("plain SET_TIMING restart", "", body_plain, 1);
    one("SET_TIMING restart behind 4 stuck frames", "", body_deferred, 1);
    one("bus-off with -a, 4 stuck frames", "-a", body_busoff, 0);
    return check_exit("test_rx_restart");
}
