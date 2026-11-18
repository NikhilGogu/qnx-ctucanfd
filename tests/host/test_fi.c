/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The fault-injection build of the driver on the host: src/ctucanfd_fi.c
 * between the driver and the fake core (fi_glue.c), and the same fake libcan
 * as test_driver.c. Injected status never reaches QEMU's interrupt line, so a
 * test client's CTUCAN_DEVCTL_FI_TICK is what brings it to the driver. These
 * cases check that it then goes through service() and finish_pulse(), the
 * code a real interrupt runs, and not through a separate dispatch.
 */
#include <time.h>
#include "driver_harness.h"

static const uint32_t ENA = INT_RBNEI | INT_DOI | INT_TXBHCI | INT_EWLI | INT_FCSI | INT_BEI;

static int fi_start(unsigned scenario, unsigned arg, unsigned arg2, unsigned budget)
{
    ctucan_fi_req_t r = { .scenario = scenario, .arg = arg, .arg2 = arg2, .budget = budget };

    return dc(RX0, CTUCAN_DEVCTL_FI, &r, sizeof(r), 0);
}

static void tick(void)
{
    struct timespec ts = { 0, 1000000 };

    nanosleep(&ts, NULL);
    CHECK_EQ(dc(RX0, CTUCAN_DEVCTL_FI_TICK, NULL, 0, 0), 0);
}

/* Ticks that ran the pulse path (CTUCAN_DEVCTL_FI_PULSES). */
static unsigned fi_pulses(void)
{
    uint32_t n = 0;

    CHECK_EQ(dc(RX0, CTUCAN_DEVCTL_FI_PULSES, &n, 0, sizeof(n)), (int)sizeof(n));
    return n;
}

/* The core's interrupt output was turned off and on again `n` times. */
static unsigned toggles(void)
{
    unsigned n = 0;

    for (unsigned i = 0; i < fake.nlog; i++) {
        n += fake.log[i].off == CTUCAN_INT_ENA_CLR && fake.log[i].val == ENA;
    }
    return n;
}

static void body_warn(void)
{
    unsigned t0 = toggles();

    CHECK_EQ(fi_start(CTUCAN_FI_WARN, 0, 0, 0), 0);
    for (int i = 0; i < 200 && xstats().ewl_down == 0; i++) {
        tick();
    }
    ctucan_xstats_t x = xstats();
    CHECK_EQ(x.ewl_up, 1);
    CHECK_EQ(x.ewl_down, 1);
    CHECK(fi_pulses() >= 2, "fi_pulses %u", fi_pulses());
    CHECK(stats().total_interrupts >= fi_pulses(), "service() ran: total_interrupts %u", stats().total_interrupts);
    CHECK_EQ(toggles() - t0, fi_pulses());   /* finish_pulse() re-armed the output each time */
    CHECK_EQ(fake.int_ena, ENA);
    CHECK_EQ(x.hw_pulses, 0);
    CHECK_EQ(lc.unmasks, 0);                /* nothing was masked for a tick */
    CHECK_EQ(x.unmask_err, 0);
    CHECK_EQ(stats().error_warning_state_count, 1);

    /* idle ticks find nothing injected and service nothing */
    unsigned n = fi_pulses();
    for (int i = 0; i < 5; i++) {
        tick();
    }
    CHECK_EQ(fi_pulses(), n);
    CHECK_EQ(xstats().spurious, 0);
}

static void body_busoff_now(void)
{
    for (unsigned i = 0; i < 2; i++) {
        CAN_MSG m = frame(0x2e9 + i, 8);
        CHECK_EQ(fake_write(TX1, &m), 0);
    }
    CHECK_EQ(xstats().state & 0xffu, 2);

    /* bus-off at once, from the request alone: no ticker, no traffic */
    CHECK_EQ(fi_start(CTUCAN_FI_BUSOFF, 2, 0, 0), 0);
    ctucan_xstats_t x = xstats();
    CHECK(busoff_hold(), "hold set");
    CHECK_EQ(x.state & 0xffu, 0);
    CHECK_EQ(x.tx_aborted, 2);
    CHECK_EQ(fi_pulses(), 1);
    CHECK_EQ(stats().bus_off_state_count, 1);
    CHECK(stats().total_interrupts >= 1, "service() ran");
    CHECK_EQ(fake.int_ena, ENA);
}

/* Bus-off with aborts that take 100 ms (arg2 bit 0) and nothing else
 * happening: the request returns at once with the TX wait on, nothing sleeps
 * on libcan's thread, and the driver's wait timer gives the aborts up at
 * 20 ms, before the injector would have finished them. */
static void body_busoff_slow_abort(void)
{
    /* as fi_busoff_abort_notick on a target: completions held for 5 s first */
    CHECK_EQ(fi_start(CTUCAN_FI_TXDELAY, 5000, 2, 0), 0);
    for (unsigned i = 0; i < 2; i++) {
        CAN_MSG m = frame(0x2e9 + i, 8);
        CHECK_EQ(fake_write(TX1, &m), 0);
    }
    uint64_t t0 = fake_now_ms();
    CHECK_EQ(fi_start(CTUCAN_FI_BUSOFF, 2, 1, 0), 0);
    ctucan_xstats_t x = xstats();
    CHECK(busoff_hold(), "hold set");
    CHECK((x.state >> 10) & 1u, "TX wait on");
    CHECK_EQ(x.state & 0xffu, 2);
    CHECK_EQ(x.tx_abort_forced, 0);
    CHECK_EQ(lc.timer_at_ms, t0 + 20);

    CHECK(fake_timer_run(), "timer fired");
    x = xstats();
    CHECK_EQ(fake_now_ms(), t0 + 20);
    CHECK_EQ(x.state & 0xffu, 0);
    CHECK_EQ((x.state >> 10) & 1u, 0);
    CHECK_EQ(x.tx_abort_forced, 2);
    CHECK_EQ(x.tx_aborted, 2);
    CHECK(busoff_hold(), "hold kept until error active");
    CHECK_EQ(lc.timer_at_ms, 0);
    CHECK_EQ(lc.slept_ns, 0);
}

static void body_txdelay(void)
{
    CHECK_EQ(fi_start(CTUCAN_FI_TXDELAY, 5, 4, 0), 0);
    for (unsigned i = 0; i < 4; i++) {
        CAN_MSG m = frame(0x2e0 + i, 8);
        CHECK_EQ(fake_write(TX1, &m), 0);
    }
    /* QEMU sends a frame the moment it is set ready; the injector shows it
     * as still in transmission until its hold ends */
    for (unsigned b = 0; b < CTUCAN_TXT_BUFFERS; b++) {
        CHECK_EQ(fake.txt[b], TXT_RDY);
        fake.txt[b] = TXT_TOK;
    }
    CHECK_EQ(xstats().tx_ok, 0);
    for (int i = 0; i < 100 && xstats().tx_ok < 4; i++) {
        tick();
    }
    ctucan_xstats_t x = xstats();
    CHECK_EQ(x.tx_ok, 4);
    CHECK_EQ(x.tx_order_err, 0);
    CHECK(fi_pulses() >= 1, "fi_pulses %u", fi_pulses());
    CHECK(stats().tx_interrupts >= 1, "TXBHCI went through service()");
    CHECK_EQ(x.state & 0xffu, 0);
    CHECK_EQ(lc.unmasks, 0);
}

static void body_budget_with_error(void)
{
    fake_block_readers(RX0, 1);
    for (unsigned i = 0; i < 3; i++) {
        rx_frame(0x2a0 + i, 8);
    }
    /* one RX frame per batch; the warning is injected by the request */
    CHECK_EQ(fi_start(CTUCAN_FI_WARN, 0, 0, 1), 0);
    ctucan_xstats_t x = xstats();
    CHECK_EQ(fi_pulses(), 1);
    CHECK_EQ(x.ewl_up, 1);
    CHECK_EQ(stats().received_frames, 1);   /* the RX budget holds next to error work */
    CHECK_EQ(x.rx_max_batch, 1);
    /* a devctl cannot wake readers: RBNEI stays set for the next interrupt */
    CHECK_EQ(lc.wakes, 0);
    CHECK(fake.mem[CTUCAN_INT_STAT / 4] & INT_RBNEI, "RBNEI held for the reader");
    CHECK_EQ(lc.dev[RX0]->msg_queue->msgcnt, 1);

    /* that interrupt's pulse wakes the reader and takes the next frame */
    fake_pulse();
    CHECK_EQ(lc.wakes, 1);
    CHECK(lc.nrx == 1 && lc.rx[0].msg.mid == 0x2a0u << 18, "reader got the oldest frame");
    CHECK_EQ(stats().received_frames, 2);
    CHECK_EQ(xstats().hw_pulses, 1);
    CHECK_EQ(lc.unmasks, 1);
}

int main(void)
{
    CHECK_EQ(run("", body_warn), 0);
    CHECK_EQ(run("", body_busoff_now), 0);
    CHECK_EQ(run("", body_busoff_slow_abort), 0);
    CHECK_EQ(run("", body_txdelay), 0);
    CHECK_EQ(run("-q 128", body_budget_with_error), 0);
    return check_exit("test_fi");
}
