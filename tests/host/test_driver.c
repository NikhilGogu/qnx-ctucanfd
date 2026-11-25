/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * src/dev-can-ctucanfd.c on the fake core and the fake libcan. The driver
 * file is included whole, with its main() renamed, so each case runs the real
 * start-up and shutdown and drives the driver only through the callbacks it
 * gave libcan: interrupt pulses, client writes and devctls. Driver state is
 * read for assertions and set directly only where noted (the TX ring
 * position, the mailbox return budget that FI builds set, the time of the
 * last register dump). The helpers are in driver_harness.h.
 */
#include "driver_harness.h"

/* ---- start and stop ---------------------------------------------------- */

static void body_start(void)
{
    const uint32_t ena = INT_RBNEI | INT_DOI | INT_TXBHCI | INT_EWLI | INT_FCSI | INT_BEI;

    CHECK(lc.intr_attached && lc.code == PULSE_CODE_IRQ, "interrupt attached with the driver's pulse code");
    CHECK(lc.dev[RX0] != NULL && lc.dev[RX0]->devtype == CANDEV_TYPE_RX, "rx0");
    CHECK(lc.dev[TX1] != NULL && lc.dev[TX1]->devtype == CANDEV_TYPE_TX, "tx1 numbered after the RX mailboxes");
    CHECK_EQ(fake.int_ena, ena);
    CHECK_EQ(fake.mem[CTUCAN_MODE / 4] & (SET_ENA | MODE_FDE), SET_ENA | MODE_FDE);
    /* the TX wait timer sends its devctl to the driver's own rx0 */
    CHECK(lc.timer_up, "TX wait timer created");
    CHECK(strcmp(lc.timer_path, "/dev/can1/rx0") == 0, "timer path %s", lc.timer_path);
    CHECK_EQ(lc.timer_dcmd, CTUCAN_DEVCTL_TIMER);
    CHECK_EQ(lc.timer_at_ms, 0);
}

static void body_start_unit(void)
{
    CHECK(strcmp(lc.timer_path, "/dev/can3/rx0") == 0, "timer path %s", lc.timer_path);
}

static void test_start_stop(void)
{
    CHECK_EQ(run("", body_start), 0);
    CHECK(lc.started, "can_resmgr_start() reached");
    CHECK(!lc.intr_attached && !lc.resmgr_up, "interrupt detached and resmgr finished after SIGTERM");
    CHECK(lc.dev[RX0] == NULL && lc.dev[TX1] == NULL, "devices destroyed");
    CHECK_EQ(fake.int_ena, 0);
    CHECK_EQ(fake.mem[CTUCAN_MODE / 4] & SET_ENA, 0);
    CHECK(!lc.timer_up, "timer deleted at shutdown");
    CHECK_EQ(lc.slept_ns, 0);   /* nothing in flight: no wait */
    CHECK_EQ(run("-u 3", body_start_unit), 0);

    /* a bad option stops the driver before it touches anything */
    CHECK_EQ(run("-T 5", body_start), EINVAL);
    CHECK(!lc.started && !lc.resmgr_up, "nothing started with -T 5");
    CHECK_EQ(run("-r 0", body_start), EINVAL);
}

/* ---- TX ---------------------------------------------------------------- */

static void body_tx_ring(void)
{
    for (unsigned i = 0; i < 6; i++) {
        CAN_MSG m = frame(0x100 + i, 8);
        CHECK_EQ(fake_write(TX1, &m), 0);
    }
    /* four buffers in use, two frames wait in libcan's queue */
    for (unsigned b = 0; b < 4; b++) {
        CHECK_EQ(fake.txt[b], TXT_RDY);
        CHECK_EQ(txt_id(b), 0x100 + b);
    }
    CHECK_EQ(lc.dev[TX1]->msg_queue->msgcnt, 2);
    CHECK_EQ(TXT_STATE(fake_last_write(CTUCAN_TX_PRIORITY), 0), 7);   /* oldest first */

    /* 0 and 1 done: retired, refilled together in one TX command */
    fake.txt[0] = fake.txt[1] = TXT_TOK;
    irq(INT_TXBHCI);
    fake_pulse();
    ctucan_xstats_t x = xstats();
    CHECK_EQ(x.tx_ok, 2);
    CHECK_EQ(x.tx_batches, 1);
    CHECK_EQ(fake_last_write(CTUCAN_TX_COMMAND), TXC_TXCR | TXC_TXB_MASK(3u));
    CHECK_EQ(txt_id(0), 0x104);
    CHECK_EQ(txt_id(1), 0x105);
    CHECK_EQ(lc.dev[TX1]->msg_queue->msgcnt, 0);
    CHECK_EQ(TXT_STATE(fake_last_write(CTUCAN_TX_PRIORITY), 2), 7);   /* buffer 2 is now the oldest */
    CHECK_EQ(fake.mem[CTUCAN_INT_STAT / 4] & INT_TXBHCI, 0);

    /* buffer 3 finishes before 2: nothing retires out of order, the break is counted */
    fake.txt[3] = TXT_TOK;
    irq(INT_TXBHCI);
    fake_pulse();
    x = xstats();
    CHECK_EQ(x.tx_ok, 2);
    CHECK_EQ(x.tx_order_err, 1);
    CHECK_EQ(fake.txt[3], TXT_TOK);
    fake.txt[2] = TXT_TOK;
    irq(INT_TXBHCI);
    fake_pulse();
    x = xstats();
    CHECK_EQ(x.tx_ok, 4);
    CHECK_EQ(x.tx_order_err, 1);    /* counted once per frame */
    CHECK_EQ(fake.txt[2], TXT_ETY);
    CHECK_EQ(fake.txt[3], TXT_ETY);

    /* final states other than "done" */
    fake.txt[0] = TXT_ERR;
    fake.txt[1] = TXT_ABT;
    irq(INT_TXBHCI);
    fake_pulse();
    x = xstats();
    CHECK_EQ(x.tx_failed, 1);
    CHECK_EQ(x.tx_aborted, 1);
    CHECK_EQ(stats().total_frame_errors, 1);
    CHECK_EQ(stats().transmitted_frames, 4);
    CHECK_EQ(xstats().state & 0xffu, 0);

    /* a buffer that lost its frame: counted, not set empty again */
    CAN_MSG m = frame(0x1aa, 1);
    CHECK_EQ(fake_write(TX1, &m), 0);
    unsigned b = g_drv.txb_tail % g_drv.ntxb;
    fake.txt[b] = TXT_ETY;
    unsigned ce = fake_write_count(CTUCAN_TX_COMMAND);
    irq(INT_TXBHCI);
    fake_pulse();
    CHECK_EQ(xstats().tx_lost, 1);
    CHECK_EQ(fake_write_count(CTUCAN_TX_COMMAND), ce);
}

static void test_tx_ring(void)
{
    CHECK_EQ(run("", body_tx_ring), 0);
}

/* Frames sent around the ring several times, starting at its last buffer:
 * each one is set ready in some TXT buffer exactly once, in the order
 * written. */
static unsigned wrap_sent[64], wrap_nsent;

static void body_tx_wrap(void)
{
    const unsigned total = 3 * g_drv.ntxb + 2;
    unsigned written = 0;

    /* Driver state set directly: the ring starts at its last buffer. The
     * first set fills every buffer, the queue fills behind it, and the refill
     * after they finish goes past the end of the ring in one tx_kick(). */
    g_drv.txb_head = g_drv.txb_tail = g_drv.ntxb - 1;
    wrap_nsent = 0;
    for (unsigned round = 0; round < 4 * total && wrap_nsent < total; round++) {
        while (written < total && lc.dev[TX1]->msg_queue->msgcnt < g_drv.ntxb) {
            CAN_MSG m = frame(0x200 + written, 4);
            CHECK_EQ(fake_write(TX1, &m), 0);
            written++;
        }
        /* the core sends the ready buffers oldest first */
        unsigned rdy[CTUCAN_TXT_BUFFERS], n = 0;
        for (unsigned b = 0; b < CTUCAN_TXT_BUFFERS; b++) {
            if (fake.txt[b] == TXT_RDY) {
                rdy[n++] = b;
            }
        }
        for (unsigned i = 1; i < n; i++) {
            for (unsigned j = i; j > 0 && txt_id(rdy[j]) < txt_id(rdy[j - 1]); j--) {
                unsigned t = rdy[j];
                rdy[j] = rdy[j - 1];
                rdy[j - 1] = t;
            }
        }
        for (unsigned i = 0; i < n && wrap_nsent < 64; i++) {
            wrap_sent[wrap_nsent++] = txt_id(rdy[i]);
            fake.txt[rdy[i]] = TXT_TOK;
        }
        irq(INT_TXBHCI);
        fake_pulse();
    }
    ctucan_xstats_t x = xstats();
    CHECK_EQ(wrap_nsent, total);
    for (unsigned i = 0; i < wrap_nsent; i++) {
        CHECK(wrap_sent[i] == 0x200 + i, "ntxb %u: frame %u sent as 0x%x", g_drv.ntxb, i, wrap_sent[i]);
    }
    CHECK_EQ(x.tx_ok, total);
    CHECK_EQ(x.tx_lost, 0);
    CHECK_EQ(x.tx_order_err, 0);
    CHECK_EQ(x.state & 0xffu, 0);
}

static void test_tx_wrap(void)
{
    static const char *const args[] = { "-T 1", "-T 2", "-T 3", "-T 4" };

    for (unsigned i = 0; i < sizeof(args) / sizeof(args[0]); i++) {
        CHECK_EQ(run(args[i], body_tx_wrap), 0);
    }
}

/* ---- errors ------------------------------------------------------------ */

#define TXW_BIT     (1u << 10)  /* GET_XSTATS state: TX wait on */
#define RESTART_BIT (1u << 11)  /* GET_XSTATS state: SET_TIMING restart pending */

static void body_busoff(void)
{
    for (unsigned i = 0; i < 2; i++) {
        CAN_MSG m = frame(0x300 + i, 8);
        CHECK_EQ(fake_write(TX1, &m), 0);
    }
    CHECK_EQ(xstats().state & 0xffu, 2);

    /* bus-off: TX held, both frames in flight aborted and counted at once */
    set_fault(FS_BOF, 256);
    fake_pulse();
    ctucan_xstats_t x = xstats();
    CHECK(busoff_hold(), "hold set at bus-off");
    CHECK_EQ(x.state & 0xffu, 0);
    CHECK_EQ(x.tx_aborted, 2);
    CHECK_EQ(stats().bus_off_state_count, 1);
    CHECK_EQ(ercrst_writes(), 0);   /* no -a: the core stays bus-off */
    CHECK_EQ(fake.mem[CTUCAN_INT_STAT / 4] & INT_FCSI, 0);

    /* frames written while held stay queued */
    CAN_MSG m = frame(0x310, 8);
    CHECK_EQ(fake_write(TX1, &m), 0);
    CHECK_EQ(lc.dev[TX1]->msg_queue->msgcnt, 1);

    /* none, then error passive: still held */
    set_fault(0, 200);
    fake_pulse();
    CHECK(busoff_hold(), "hold kept when FAULT_STATE reads none");
    set_fault(FS_ERP, 127);
    fake_pulse();
    CHECK(busoff_hold(), "hold kept at error passive");
    CHECK_EQ(stats().error_passive_state_count, 1);
    CHECK_EQ(lc.dev[TX1]->msg_queue->msgcnt, 1);
    for (unsigned b = 0; b < CTUCAN_TXT_BUFFERS; b++) {
        CHECK(fake.txt[b] != TXT_RDY, "buffer %u set ready while held", b);
    }

    /* error active: released, the queued frame goes out */
    set_fault(FS_ERA, 0);
    fake_pulse();
    CHECK(!busoff_hold(), "hold released at error active");
    CHECK_EQ(xstats().busoff_recovered, 1);
    CHECK_EQ(lc.dev[TX1]->msg_queue->msgcnt, 0);
    CHECK_EQ(xstats().state & 0xffu, 1);

    /* manual recovery writes ERCRST only while bus-off */
    CHECK_EQ(fake_devctl(RX0, CTUCAN_DEVCTL_RECOVER, NULL, 0, 0), 0);
    CHECK_EQ(ercrst_writes(), 0);
    set_fault(FS_BOF, 256);
    fake_pulse();
    CHECK_EQ(fake_devctl(RX0, CTUCAN_DEVCTL_RECOVER, NULL, 0, 0), 0);
    CHECK_EQ(ercrst_writes(), 1);
    CHECK_EQ(stats().bus_off_state_count, 2);
}

static void body_busoff_auto(void)
{
    set_fault(FS_BOF, 256);
    fake_pulse();
    CHECK_EQ(ercrst_writes(), 1);
    CHECK(busoff_hold(), "hold set");
    /* the same state again is no new bus-off */
    irq(INT_FCSI);
    fake_pulse();
    CHECK_EQ(stats().bus_off_state_count, 1);
    CHECK_EQ(ercrst_writes(), 1);
}

/* FAULT_STATE reads bus-off before the FCSI that shows it is serviced: a TXBHCI
 * pass and client writes come first. No Set ready must go out in that time;
 * then the FCSI pass does the usual bus-off work, and after recovery the
 * queued frames go in order. */
static unsigned tx_cmd_writes(void)
{
    return fake_write_count(CTUCAN_TX_COMMAND);
}

static void body_busoff_before_fcsi(void)
{
    fake.mem[CTUCAN_EWL_ERP_FAULT / 4] = FS_BOF | 96u;     /* bus-off, and no FCSI pending yet */
    fake.mem[CTUCAN_REC_TEC / 4] = 256u << 16;
    for (unsigned i = 0; i < 2; i++) {
        CAN_MSG m = frame(0x400 + i, 8);
        CHECK_EQ(fake_write(TX1, &m), 0);
    }
    CHECK_EQ(tx_cmd_writes(), 0);
    CHECK_EQ(lc.dev[TX1]->msg_queue->msgcnt, 2);
    CHECK_EQ(xstats().state & 0xffu, 0);

    irq(INT_TXBHCI);            /* TX interrupt first, as two separate interrupts */
    fake_pulse();
    CHECK_EQ(tx_cmd_writes(), 0);
    CHECK_EQ(lc.dev[TX1]->msg_queue->msgcnt, 2);
    CHECK(!busoff_hold(), "no bus-off seen yet");
    CHECK_EQ(stats().bus_off_state_count, 0);
    for (unsigned b = 0; b < CTUCAN_TXT_BUFFERS; b++) {
        CHECK(fake.txt[b] != TXT_RDY && fake.txt[b] != TXT_ABT, "buffer %u used while bus-off", b);
    }

    irq(INT_FCSI);              /* now the FCSI: the usual bus-off handling */
    fake_pulse();
    CHECK(busoff_hold(), "hold set at the FCSI");
    CHECK_EQ(stats().bus_off_state_count, 1);
    CHECK_EQ(xstats().tx_aborted, 0);
    CHECK_EQ(tx_cmd_writes(), 0);
    CHECK_EQ(lc.dev[TX1]->msg_queue->msgcnt, 2);

    set_fault(FS_ERA, 0);
    fake_pulse();
    CHECK(!busoff_hold(), "hold released at error active");
    CHECK_EQ(xstats().busoff_recovered, 1);
    CHECK_EQ(stats().bus_off_state_count, 1);
    CHECK_EQ(lc.dev[TX1]->msg_queue->msgcnt, 0);
    CHECK_EQ(tx_cmd_writes(), 1);
    CHECK_EQ(txt_id(0), 0x400);
    CHECK_EQ(txt_id(1), 0x401);
    CHECK_EQ(fake.txt[0], TXT_RDY);
    CHECK_EQ(fake.txt[1], TXT_RDY);
}

static void test_busoff(void)
{
    CHECK_EQ(run("", body_busoff), 0);
    CHECK_EQ(run("", body_busoff_before_fcsi), 0);
    CHECK_EQ(run("-a", body_busoff_auto), 0);
}

static void body_error_states(void)
{
    /* warning limit, up and down, from STATUS.EWL; FAULT_STATE stays active */
    fake.mem[CTUCAN_STATUS / 4] |= STAT_EWL;
    irq(INT_EWLI);
    fake_pulse();
    fake.mem[CTUCAN_STATUS / 4] &= ~STAT_EWL;
    irq(INT_EWLI);
    fake_pulse();
    irq(INT_EWLI);      /* no change: not counted */
    fake_pulse();
    ctucan_xstats_t x = xstats();
    CAN_DEVCTL_STATS s = stats();
    CHECK_EQ(x.ewl_up, 1);
    CHECK_EQ(x.ewl_down, 1);
    CHECK_EQ(s.error_warning_state_count, 1);
    CHECK_EQ(s.error_passive_state_count, 0);
    CHECK_EQ(fake.mem[CTUCAN_INT_STAT / 4] & INT_EWLI, 0);

    /* bus errors by type */
    static const unsigned types[] = { ERRT_BIT, ERRT_CRC, ERRT_FORM, ERRT_ACK, ERRT_STUFF, ERRT_ACK, 7u };
    for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        fake.mem[CTUCAN_ERR_CAPT / 4] = types[i] << 5;
        irq(INT_BEI);
        fake_pulse();
    }
    s = stats();
    CHECK_EQ(s.dom_bit_recess_errors, 1);
    CHECK_EQ(s.crc_errors, 1);
    CHECK_EQ(s.form_errors, 1);
    CHECK_EQ(s.missing_ack, 2);
    CHECK_EQ(s.stuff_errors, 1);
    CHECK_EQ(s.total_frame_errors, 7);

    /* overrun: data overrun cleared before its interrupt is acknowledged */
    unsigned first = fake.nlog;
    irq(INT_DOI);
    fake_pulse();
    CHECK_EQ(stats().hw_receive_overflows, 1);
    unsigned cdo = fake.nlog, ack = fake.nlog;
    for (unsigned i = first; i < fake.nlog; i++) {
        if (cdo == fake.nlog && fake.log[i].off == CTUCAN_COMMAND && fake.log[i].val == CMD_CDO) {
            cdo = i;
        }
        if (ack == fake.nlog && fake.log[i].off == CTUCAN_INT_STAT && (fake.log[i].val & INT_DOI)) {
            ack = i;
        }
    }
    CHECK(cdo < ack && ack < fake.nlog, "COMMAND.CDO at %u before the DOI ack at %u", cdo, ack);
}

static void test_error_states(void)
{
    CHECK_EQ(run("", body_error_states), 0);
}

/* ---- interrupt service ------------------------------------------------- */

static void body_service(void)
{
    const uint32_t ena = fake.int_ena;

    /* nothing pending: counted as spurious, unmasked once with the output off */
    fake_pulse();
    ctucan_xstats_t x = xstats();
    CHECK_EQ(x.hw_pulses, 1);
    CHECK_EQ(x.spurious, 1);
    CHECK_EQ(lc.unmasks, 1);
    CHECK_EQ(lc.ena_at_unmask, 0);
    CHECK_EQ(fake.int_ena, ena);

    /* 100 frames, no reader: 64 per pulse, the rest left for the next */
    for (unsigned i = 0; i < 100; i++) {
        rx_frame(0x400 + (i & 0xff), 8);
    }
    fake_pulse();
    x = xstats();
    CHECK_EQ(stats().received_frames, 64);
    CHECK_EQ(x.rx_max_batch, 64);
    CHECK_EQ(x.deferred, 1);
    CHECK_EQ(x.rearm_pending, 1);
    CHECK_EQ(fake.rx_frames, 36);
    fake_pulse();
    CHECK_EQ(stats().received_frames, 100);
    CHECK_EQ(fake.rx_frames, 0);
    CHECK_EQ(fake.mem[CTUCAN_INT_STAT / 4] & INT_RBNEI, 0);
    CHECK_EQ(lc.dev[RX0]->msg_queue->msgcnt, 100);

    /* a source that sets itself again at every ack: at most 8 passes per pulse */
    unsigned before = stats().total_interrupts;
    fake.int_storm = INT_BEI;
    fake_pulse();
    fake.int_storm = 0;
    fake.mem[CTUCAN_INT_STAT / 4] = 0;
    CHECK_EQ(stats().total_interrupts - before, PASS_LIMIT);
    CHECK_EQ(lc.unmasks, 4);
    CHECK_EQ(xstats().rearm_pending, 2);

    CHECK(lc.ena_off_at_every_unmask, "the core's output was off at every InterruptUnmask()");
    CHECK_EQ(fake.int_ena, ena);

    /* a refused unmask is counted and logged once */
    lc.unmask_rc = -1;
    unsigned lines = lc.slog_lines;
    fake_pulse();
    fake_pulse();
    lc.unmask_rc = 0;
    CHECK_EQ(xstats().unmask_err, 2);
    CHECK_EQ(lc.slog_lines - lines, 1);
}

static void body_stopped(void)
{
    CAN_DEVCTL_TIMING t = { .bit_rate_prescaler = 4, .time_segment_1 = 43, .time_segment_2 = 6,
                            .sync_jump_width = 6 };

    /* a restart whose reset never completes leaves the core stopped */
    fake.reset_stuck = true;
    errno = 0;
    CHECK_EQ(fake_devctl(RX0, CAN_DEVCTL_SET_TIMING, &t, sizeof(t), 0), (unsigned long long)-1);
    CHECK_EQ(errno, ETIMEDOUT);
    CHECK_EQ(xstats().restart_fail, 1);
    CHECK_EQ(fake.int_ena, 0);

    /* pending status in a stopped core: unmasked, output left off, not serviced */
    rx_frame(0x123, 8);
    fake_pulse();
    ctucan_xstats_t x = xstats();
    CHECK_EQ(x.stopped_pulses, 1);
    CHECK_EQ(stats().received_frames, 0);
    CHECK_EQ(lc.unmasks, 1);
    CHECK_EQ(fake.int_ena, 0);

    /* the next restart works */
    fake.reset_stuck = false;
    CHECK_EQ(fake_devctl(RX0, CAN_DEVCTL_SET_TIMING, &t, sizeof(t), 0), 0);
    CHECK_EQ(xstats().restarts, 1);
    CHECK(fake.int_ena != 0, "output on after the restart");
}

/* PCI posts memory writes: the INT_ENA_CLR in finish_pulse() may still be
 * on its way when the line is unmasked, unless a read from the core follows. */
static void body_posted(void)
{
    fake.posted = true;
    rx_frame(0x123, 8);
    fake_pulse();
    fake_pulse();
    CHECK_EQ(stats().received_frames, 1);
    CHECK_EQ(lc.unmasks, 2);
    CHECK(lc.ena_off_at_every_unmask, "INT_ENA was 0x%x at InterruptUnmask()", lc.ena_at_unmask);
    fake.posted = false;
}

static void test_service(void)
{
    CHECK_EQ(run("", body_posted), 0);
    CHECK_EQ(run("-q 128", body_service), 0);
    CHECK_EQ(run("", body_stopped), 0);
}

/* ---- reader wake-ups --------------------------------------------------- */

static void body_reports(void)
{
    fake_block_readers(0, 1);
    fake_block_readers(1, 2);
    for (unsigned i = 0; i < 3; i++) {
        rx_frame(0x500 + i, 2);
    }
    fake_pulse();
    /* every mailbox takes every frame; each return wakes one reader */
    CHECK_EQ(lc.wakes, 3);
    CHECK_EQ(lc.dev[0]->wait_client_queue->cnt, 0);
    CHECK_EQ(lc.dev[1]->wait_client_queue->cnt, 0);
    CHECK_EQ(lc.dev[0]->msg_queue->msgcnt, 2);
    CHECK_EQ(lc.dev[1]->msg_queue->msgcnt, 1);
    CHECK(lc.nrx == 3 && lc.rx[0].msg.mid == 0x500u << 18, "first reader got the oldest frame");
    CHECK_EQ(fake.mem[CTUCAN_INT_STAT / 4] & INT_RBNEI, 0);

    /* driver state set directly: one return per pulse, as FI builds allow */
    g_drv.report_budget = 1;
    fake_block_readers(0, 1);
    fake_block_readers(1, 1);
    rx_frame(0x510, 2);
    unsigned wakes = lc.wakes;
    fake_pulse();
    CHECK_EQ(lc.wakes - wakes, 1);
    CHECK_EQ(xstats().notify_deferred, 1);
    CHECK(fake.mem[CTUCAN_INT_STAT / 4] & INT_RBNEI, "RBNEI held for the reader left");
    fake_pulse();
    CHECK_EQ(lc.wakes - wakes, 2);
    CHECK_EQ(fake.mem[CTUCAN_INT_STAT / 4] & INT_RBNEI, 0);
}

static void body_stuck(void)
{
    /* returns that wake nobody: retried for STUCK_LIMIT pulses, then dropped */
    lc.nowake = true;
    fake_block_readers(0, 1);
    rx_frame(0x600, 1);
    rx_frame(0x601, 1);
    unsigned pulses = 0;
    while (xstats().notify_stuck == 0 && pulses < 20) {
        fake_pulse();
        pulses++;
    }
    ctucan_xstats_t x = xstats();
    CHECK_EQ(pulses, STUCK_LIMIT);
    CHECK_EQ(x.rx_discarded, 2);
    CHECK_EQ(lc.dev[0]->msg_queue->msgcnt, 0);
    CHECK_EQ(fake.mem[CTUCAN_INT_STAT / 4] & INT_RBNEI, 0);
    unsigned returns = lc.returns;
    fake_pulse();
    CHECK_EQ(lc.returns, returns);

    /* wake-ups work again: the same reader gets the next frame */
    lc.nowake = false;
    rx_frame(0x602, 1);
    fake_pulse();
    CHECK(lc.nrx == 1 && lc.rx[0].msg.mid == 0x602u << 18, "reader got the next frame");
}

static void test_reports(void)
{
    CHECK_EQ(run("-r 2", body_reports), 0);
    CHECK_EQ(run("", body_stuck), 0);
}

/* ---- retransmission limit and bus error interrupt --------------------- */

static void body_retr_limit(void)
{
    uint32_t mode = fake.mem[CTUCAN_MODE / 4];

    CHECK(mode & SET_RTRLE, "RTRLE set with -R 3");
    CHECK_EQ((mode >> SET_RTRTH_SHIFT) & 0xfu, 3);

    /* a frame nobody acknowledged ends in TX failed; the next one goes */
    for (unsigned i = 0; i < 2; i++) {
        CAN_MSG m = frame(0x700 + i, 8);
        CHECK_EQ(fake_write(TX1, &m), 0);
    }
    fake.txt[0] = TXT_ERR;
    irq(INT_TXBHCI);
    fake_pulse();
    ctucan_xstats_t x = xstats();
    CHECK_EQ(x.tx_failed, 1);
    CHECK_EQ(x.state & 0xffu, 1);
    CHECK_EQ(stats().total_frame_errors, 1);
}

static void body_no_bei(void)
{
    CHECK_EQ(fake.int_ena & INT_BEI, 0);
    CHECK(fake_last_write(CTUCAN_INT_MASK_SET) & INT_BEI, "BEI masked, so it does not latch");
    CHECK(fake.int_ena & INT_FCSI, "fault confinement still reported");

    /* even if BEI were latched, it is not serviced */
    fake.mem[CTUCAN_ERR_CAPT / 4] = ERRT_ACK << 5;
    irq(INT_BEI);
    fake_pulse();
    CHECK_EQ(stats().missing_ack, 0);
    CHECK_EQ(xstats().spurious, 1);
}

static void body_defaults_one_shot(void)
{
    CHECK(fake.mem[CTUCAN_MODE / 4] & SET_RTRLE, "RTRLE set with -R 0");
    CHECK_EQ((fake.mem[CTUCAN_MODE / 4] >> SET_RTRTH_SHIFT) & 0xfu, 0);
}

static void body_defaults(void)
{
    CHECK_EQ(fake.mem[CTUCAN_MODE / 4] & (SET_RTRLE | 0xfu << SET_RTRTH_SHIFT), 0);
    CHECK(fake.int_ena & INT_BEI, "BEI on by default");
}

static void test_retr_limit(void)
{
    CHECK_EQ(run("", body_defaults), 0);
    CHECK_EQ(run("-R 3", body_retr_limit), 0);
    CHECK_EQ(run("-R 0", body_defaults_one_shot), 0);
    CHECK_EQ(run("-R 16", body_defaults), EINVAL);
    CHECK_EQ(run("-E", body_no_bei), 0);
}

/* ---- devctl ------------------------------------------------------------ */

/* One byte short is refused with EINVAL; the full size is not. */
static void size_check(unsigned dev, int dcmd, void *data, size_t size, const char *name)
{
    CHECK(dc(dev, dcmd, data, size - 1, 0) == -1 && errno == EINVAL, "%s with %zu bytes", name, size - 1);
    int rc = dc(dev, dcmd, data, size, 0);
    CHECK(rc != -1 || errno != EINVAL, "%s with %zu bytes refused", name, size);
}

static void body_devctl(void)
{
    CAN_DCMD_DATA d;
    CAN_DEVCTL_TIMING t = { .bit_rate_prescaler = 4, .time_segment_1 = 43, .time_segment_2 = 6,
                            .sync_jump_width = 6 };
    CAN_MSG m = frame(0x123, 8);
    ctucan_fi_req_t fi = { 0 };

    memset(&d, 0, sizeof(d));
    size_check(TX1, CAN_DEVCTL_SET_MID, &d, sizeof(d.mid), "SET_MID");
    size_check(RX0, CAN_DEVCTL_SET_MFILTER, &d, sizeof(d.mfilter), "SET_MFILTER");
    size_check(RX0, CAN_DEVCTL_SET_TIMESTAMP, &d, sizeof(d.timestamp), "SET_TIMESTAMP");
    size_check(RX0, CAN_DEVCTL_SET_TIMING, &t, sizeof(t), "SET_TIMING");
    unsigned restarts = xstats().restarts;
    size_check(TX1, CAN_DEVCTL_WRITE_CANMSG_EXT, &m, sizeof(m), "WRITE_CANMSG_EXT");
    size_check(TX1, CAN_DEVCTL_TX_FRAME_RAW, &m, sizeof(m), "TX_FRAME_RAW");

    /* SET_MID keeps the identifier format of the mailbox */
    d.mid = 0xffffffffu;
    CHECK_EQ(dc(TX1, CAN_DEVCTL_SET_MID, &d, sizeof(d.mid), 0), 0);
    CHECK_EQ(dc(TX1, CAN_DEVCTL_GET_MID, &d, 0, sizeof(d.mid)), sizeof(d.mid));
    CHECK_EQ(d.mid, IDW_STD_MASK);

    /* filters are for RX mailboxes only */
    CHECK(dc(TX1, CAN_DEVCTL_GET_MFILTER, &d, 0, 0) == -1 && errno == EINVAL, "GET_MFILTER on tx");
    CHECK(dc(TX1, CAN_DEVCTL_SET_MFILTER, &d, sizeof(d.mfilter), 0) == -1 && errno == EINVAL, "SET_MFILTER on tx");
    d.mfilter = 0xffffffffu;
    CHECK_EQ(dc(RX0, CAN_DEVCTL_SET_MFILTER, &d, sizeof(d.mfilter), 0), 0);
    CHECK_EQ(dc(RX0, CAN_DEVCTL_GET_MFILTER, &d, 0, sizeof(d.mfilter)), sizeof(d.mfilter));
    CHECK_EQ(d.mfilter, IDW_EXT_MASK);

    /* frames sent through a devctl: checked here, queued by libcan */
    CHECK(dc(RX0, CAN_DEVCTL_WRITE_CANMSG_EXT, &m, sizeof(m), 0) == -1 && errno == EINVAL, "write on rx");
    CHECK(dc(TX1, CAN_DEVCTL_WRITE_CANMSG_EXT, &m, sizeof(m), 0) == -1 && errno == ENOTSUP, "write on tx");
    m.len = 65;
    CHECK(dc(TX1, CAN_DEVCTL_TX_FRAME_RAW, &m, sizeof(m), 0) == -1 && errno == EINVAL, "65 bytes");
    m.len = 9;
    m.ext.is_remote_frame = 1;
    CHECK(dc(TX1, CAN_DEVCTL_TX_FRAME_RAW, &m, sizeof(m), 0) == -1 && errno == EINVAL, "9-byte remote frame");
    CHECK(dc(TX1, CAN_DEVCTL_READ_CANMSG_EXT, &m, 0, 0) == -1 && errno == EINVAL, "read on tx");
    CHECK(dc(RX0, CAN_DEVCTL_RX_FRAME_RAW_NOBLOCK, &m, 0, 0) == -1 && errno == ENOTSUP, "read on rx");

    /* timestamps follow the value set */
    d.timestamp = 123456789u;
    CHECK_EQ(dc(RX0, CAN_DEVCTL_SET_TIMESTAMP, &d, sizeof(d.timestamp), 0), 0);
    CHECK_EQ(dc(RX0, CAN_DEVCTL_GET_TIMESTAMP, &d, 0, sizeof(d.timestamp)), sizeof(d.timestamp));
    CHECK(d.timestamp - 123456789u < 1000000u, "timestamp %u", d.timestamp);

    /* timing: refused values change nothing, a valid one restarts the core */
    CAN_DEVCTL_TIMING bad = t;
    bad.bit_rate_prescaler = 0;
    CHECK(dc(RX0, CAN_DEVCTL_SET_TIMING, &bad, sizeof(bad), 0) == -1 && errno == EINVAL, "brp 0");
    bad = t;
    bad.sync_jump_width = 7;        /* above phase segment 2 */
    CHECK(dc(RX0, CAN_DEVCTL_SET_TIMING, &bad, sizeof(bad), 0) == -1 && errno == EINVAL, "sjw > ph2");
    bad = t;
    bad.bit_rate_prescaler = 1;     /* 2 Mbit/s nominal */
    CHECK(dc(RX0, CAN_DEVCTL_SET_TIMING, &bad, sizeof(bad), 0) == -1 && errno == EINVAL, "2 Mbit/s nominal");
    CHECK_EQ(xstats().restarts, restarts);
    t.bit_rate_prescaler = 8;
    CHECK_EQ(dc(RX0, CAN_DEVCTL_SET_TIMING, &t, sizeof(t), 0), 0);
    CHECK_EQ(xstats().restarts, restarts + 1);
    CHECK_EQ(dc(RX0, CAN_DEVCTL_GET_INFO, &d, 0, sizeof(d.info)), sizeof(d.info));
    CHECK_EQ(d.info.bit_rate, 250000);
    CHECK_EQ(d.info.bit_rate_prescaler, 8);
    CHECK_EQ(d.info.time_segment_1, 43);
    CHECK_EQ(d.info.time_segment_2, 6);
    CHECK_EQ((fake.mem[CTUCAN_BTR / 4] >> BTR_BRP_SHIFT) & 0xffu, 8);
    CHECK_EQ(fake.mem[CTUCAN_MODE / 4] & SET_ENA, SET_ENA);

    /* the register dump: at most once a second, so no client can flood slog2 */
    unsigned lines = lc.slog_lines;
    CHECK_EQ(dc(RX0, CAN_DEVCTL_DEBUG_INFO, NULL, 0, 0), 0);
    unsigned dump = lc.slog_lines - lines;
    CHECK(dump >= 16, "%u register lines", dump);
    lines = lc.slog_lines;
    CHECK(dc(TX1, CAN_DEVCTL_DEBUG_INFO2, NULL, 0, 0) == -1 && errno == EAGAIN, "second dump within a second");
    CHECK_EQ(lc.slog_lines, lines);
    g_drv.dump_ms -= 1000;  /* driver state set directly: the last dump a second ago */
    CHECK_EQ(dc(RX0, CAN_DEVCTL_DEBUG_INFO, NULL, 0, 0), 0);
    CHECK_EQ(lc.slog_lines - lines, dump);
    CHECK_EQ(dc(RX0, CAN_DEVCTL_ERROR, &d, 0, sizeof(d.error)), sizeof(d.error));
    CHECK_EQ(d.error.drvr1 & FS_MASK, 0);    /* the fake core reads FAULT_STATE 0, as QEMU 6.2 does */
    CHECK(dc(RX0, CTUCAN_DEVCTL_FI, &fi, sizeof(fi), 0) == -1 && errno == ENOTSUP, "FI on a normal build");
    CHECK(dc(RX0, CTUCAN_DEVCTL_FI_TICK, NULL, 0, 0) == -1 && errno == ENOTSUP, "FI_TICK on a normal build");
    CHECK(dc(RX0, __DION(_DCMD_MISC, 99), NULL, 0, 0) == -1 && errno == ENOTSUP, "unknown command");
    /* the timer's devctl with no TX wait on: accepted, changes nothing */
    restarts = xstats().restarts;
    CHECK_EQ(dc(TX1, CTUCAN_DEVCTL_TIMER, NULL, 0, 0), 0);
    CHECK_EQ(xstats().restarts, restarts);
    CHECK_EQ(xstats().state & (TXW_BIT | RESTART_BIT), 0);
}

static void body_devctl_fd_always(void)
{
    CAN_MSG m = frame(0x123, 8);

    m.ext.is_remote_frame = 1;
    CHECK(dc(TX1, CAN_DEVCTL_TX_FRAME_RAW, &m, sizeof(m), 0) == -1 && errno == EINVAL, "remote frame with -A");
}

static void test_devctl(void)
{
    CHECK_EQ(run("", body_devctl), 0);
    CHECK_EQ(run("-A", body_devctl_fd_always), 0);
}


/* ---- TX wait (docs/DESIGN.md, section 5) --------------------------------- */

/* Every case checks lc.slept_ns: libcan's thread never sleeps, and the waits
 * end by interrupt pulse, client devctl or the timer's devctl alone. */

static const CAN_DEVCTL_TIMING T250 = { .bit_rate_prescaler = 8, .time_segment_1 = 43, .time_segment_2 = 6,
                                        .sync_jump_width = 6 };

static unsigned btr_brp(void)
{
    return (fake.mem[CTUCAN_BTR / 4] >> BTR_BRP_SHIFT) & 0xffu;
}

static void write_frames(uint32_t id, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        CAN_MSG m = frame(id + i, 8);
        CHECK_EQ(fake_write(TX1, &m), 0);
    }
}

/* Bus-off with aborts that do not finish at once: the wait does not block
 * the pulse, an interrupt retires what does finish, the timer gives up the
 * rest. The hold still ends only at error active, and TX resumes only once
 * the wait is over too. */
static void body_busoff_wait(void)
{
    write_frames(0x300, 2);
    fake.abort_slow = true;
    uint64_t t0 = fake_now_ms();
    set_fault(FS_BOF, 256);
    fake_pulse();
    ctucan_xstats_t x = xstats();
    CHECK(busoff_hold(), "hold set at bus-off");
    CHECK_EQ(x.state & 0xffu, 2);
    CHECK_EQ(x.state & TXW_BIT, TXW_BIT);
    CHECK_EQ(fake.txt[0], TXT_ABTP);
    CHECK_EQ(fake.txt[1], TXT_ABTP);
    CHECK_EQ(x.tx_aborted, 0);
    CHECK_EQ(lc.timer_at_ms, t0 + TX_QUIESCE_MS);

    /* buffer 0's abort completes: its interrupt retires it, the wait goes on */
    fake.txt[0] = TXT_ABT;
    irq(INT_TXBHCI);
    fake_pulse();
    x = xstats();
    CHECK_EQ(x.tx_aborted, 1);
    CHECK_EQ(x.state & 0xffu, 1);
    CHECK_EQ(x.state & TXW_BIT, TXW_BIT);

    /* error active releases the hold; a frame written now still waits */
    CAN_MSG m = frame(0x310, 8);
    CHECK_EQ(fake_write(TX1, &m), 0);
    set_fault(FS_ERA, 0);
    fake_pulse();
    CHECK(!busoff_hold(), "hold released at error active");
    CHECK_EQ(lc.dev[TX1]->msg_queue->msgcnt, 1);

    /* an early wake-up, from the timer or any client, changes nothing */
    fake_advance_ms(TX_QUIESCE_MS - 1);
    CHECK(!fake_timer_fire(), "timer not due 19 ms in");
    CHECK_EQ(dc(TX1, CTUCAN_DEVCTL_TIMER, NULL, 0, 0), 0);
    CHECK_EQ(xstats().state & 0xffu, 1);

    /* at the deadline the timer's devctl gives buffer 1 up and TX resumes */
    CHECK(fake_timer_run(), "timer armed");
    CHECK_EQ(lc.timer_fired, 1);
    x = xstats();
    CHECK_EQ(x.tx_abort_forced, 1);
    CHECK_EQ(x.tx_aborted, 2);
    CHECK_EQ(x.state & (TXW_BIT | RESTART_BIT | 0xffu), 1);    /* the queued frame is in flight */
    CHECK_EQ(lc.timer_at_ms, 0);
    CHECK_EQ(lc.dev[TX1]->msg_queue->msgcnt, 0);
    CHECK_EQ(fake.txt[2], TXT_RDY);
    CHECK_EQ(txt_id(2), 0x310);
    CHECK_EQ(ercrst_writes(), 0);
    CHECK_EQ(lc.slept_ns, 0);
}

/* -a: recovery is requested when the wait for the aborts ends, here by the
 * interrupt that retires the last of them */
static void body_busoff_wait_auto(void)
{
    write_frames(0x320, 1);
    fake.abort_slow = true;
    set_fault(FS_BOF, 256);
    fake_pulse();
    CHECK_EQ(ercrst_writes(), 0);
    CHECK_EQ(xstats().state & TXW_BIT, TXW_BIT);
    fake.txt[0] = TXT_ABT;
    irq(INT_TXBHCI);
    fake_pulse();
    CHECK_EQ(ercrst_writes(), 1);
    CHECK_EQ(xstats().state & (TXW_BIT | 0xffu), 0);
    CHECK_EQ(lc.timer_at_ms, 0);
    CHECK(busoff_hold(), "hold kept until error active");
    CHECK_EQ(lc.slept_ns, 0);
}

/* SET_TIMING with frames in flight: replied at once, restart pending; the
 * interrupt that retires the last frame ends the wait and restarts. */
static void body_restart_irq(void)
{
    CAN_DEVCTL_TIMING t = T250;

    write_frames(0x400, 2);
    unsigned restarts = xstats().restarts, brp = btr_brp();
    CHECK_EQ(dc(RX0, CAN_DEVCTL_SET_TIMING, &t, sizeof(t), 0), 0);
    ctucan_xstats_t x = xstats();
    CHECK_EQ(x.restarts, restarts);
    CHECK_EQ(x.state & (TXW_BIT | RESTART_BIT), TXW_BIT | RESTART_BIT);
    CHECK_EQ(lc.timer_at_ms, fake_now_ms() + TX_QUIESCE_MS);
    CHECK_EQ(btr_brp(), brp);
    CHECK(strstr(lc.last_log, "restart once the TXT buffers are done") != NULL, "%s", lc.last_log);

    /* writes while waiting stay queued */
    CAN_MSG m = frame(0x410, 8);
    CHECK_EQ(fake_write(TX1, &m), 0);
    CHECK_EQ(lc.dev[TX1]->msg_queue->msgcnt, 1);
    CHECK_EQ(fake.txt[2], TXT_ETY);

    fake.txt[0] = fake.txt[1] = TXT_TOK;
    irq(INT_TXBHCI);
    fake_pulse();
    x = xstats();
    CHECK_EQ(x.tx_ok, 2);
    CHECK_EQ(x.tx_aborted, 0);
    CHECK_EQ(x.restarts, restarts + 1);
    CHECK_EQ(x.state & (TXW_BIT | RESTART_BIT), 0);
    CHECK_EQ(lc.timer_at_ms, 0);
    CHECK_EQ(btr_brp(), 8);
    CHECK(fake.int_ena != 0, "output on after the restart");
    /* the queued frame goes out on the restarted core, from buffer 0 */
    CHECK_EQ(lc.dev[TX1]->msg_queue->msgcnt, 0);
    CHECK_EQ(fake.txt[0], TXT_RDY);
    CHECK_EQ(txt_id(0), 0x410);
    CHECK_EQ(lc.slept_ns, 0);
}

/* Frames that never finish and aborts that never finish: the first deadline
 * aborts, the second gives up, both through the timer's devctl. */
static void body_restart_deadline(void)
{
    CAN_DEVCTL_TIMING t = T250;

    write_frames(0x420, 2);
    fake.abort_slow = true;
    unsigned restarts = xstats().restarts;
    unsigned cmds = fake_write_count(CTUCAN_TX_COMMAND);
    CHECK_EQ(dc(RX0, CAN_DEVCTL_SET_TIMING, &t, sizeof(t), 0), 0);
    fake_advance_ms(TX_QUIESCE_MS - 1);
    CHECK(!fake_timer_fire(), "not due");
    CHECK_EQ(fake_write_count(CTUCAN_TX_COMMAND), cmds);   /* no abort before the deadline */

    CHECK(fake_timer_run(), "armed");
    CHECK_EQ(fake_last_write(CTUCAN_TX_COMMAND), TXC_TXCA | TXC_TXB_MASK(3u));
    ctucan_xstats_t x = xstats();
    CHECK_EQ(x.restarts, restarts);
    CHECK_EQ(x.state & (TXW_BIT | RESTART_BIT | 0xffu), TXW_BIT | RESTART_BIT | 2);
    CHECK_EQ(lc.timer_at_ms, fake_now_ms() + TX_QUIESCE_MS);

    CHECK(fake_timer_run(), "armed again");
    x = xstats();
    CHECK_EQ(x.tx_abort_forced, 2);
    CHECK_EQ(x.tx_aborted, 2);
    CHECK_EQ(x.restarts, restarts + 1);
    CHECK_EQ(x.state & (TXW_BIT | RESTART_BIT | 0xffu), 0);
    CHECK_EQ(lc.timer_at_ms, 0);
    CHECK_EQ(btr_brp(), 8);
    CHECK_EQ(lc.timer_fired, 2);
    CHECK_EQ(lc.slept_ns, 0);
}

/* As on QEMU, the abort finishes at once: the first deadline ends the wait. */
static void body_restart_abort(void)
{
    CAN_DEVCTL_TIMING t = T250;

    write_frames(0x430, 2);
    unsigned restarts = xstats().restarts;
    CHECK_EQ(dc(RX0, CAN_DEVCTL_SET_TIMING, &t, sizeof(t), 0), 0);
    CHECK(fake_timer_run(), "armed");
    ctucan_xstats_t x = xstats();
    CHECK_EQ(x.tx_aborted, 2);
    CHECK_EQ(x.tx_abort_forced, 0);
    CHECK_EQ(x.restarts, restarts + 1);
    CHECK_EQ(x.state & (TXW_BIT | RESTART_BIT | 0xffu), 0);
    CHECK_EQ(lc.slept_ns, 0);
}

/* The latest timing wins; a deferred reset that fails is counted and logged. */
static void body_restart_latest(void)
{
    CAN_DEVCTL_TIMING t = T250;

    write_frames(0x440, 1);
    unsigned restarts = xstats().restarts;
    CHECK_EQ(dc(RX0, CAN_DEVCTL_SET_TIMING, &t, sizeof(t), 0), 0);
    t.bit_rate_prescaler = 5;
    CHECK_EQ(dc(RX0, CAN_DEVCTL_SET_TIMING, &t, sizeof(t), 0), 0);
    CHECK_EQ(xstats().restarts, restarts);
    fake.txt[0] = TXT_TOK;
    irq(INT_TXBHCI);
    fake_pulse();
    CHECK_EQ(xstats().restarts, restarts + 1);
    CHECK_EQ(btr_brp(), 5);

    write_frames(0x450, 1);
    fake.reset_stuck = true;
    CHECK_EQ(dc(RX0, CAN_DEVCTL_SET_TIMING, &t, sizeof(t), 0), 0);
    fake.txt[0] = TXT_TOK;
    irq(INT_TXBHCI);
    fake_pulse();
    ctucan_xstats_t x = xstats();
    CHECK_EQ(x.restart_fail, 1);
    CHECK_EQ(x.restarts, restarts + 1);
    CHECK_EQ(x.state & (TXW_BIT | RESTART_BIT), 0);
    CHECK_EQ(fake.int_ena, 0);
    CHECK(strstr(lc.last_log, "deferred restart failed") != NULL, "%s", lc.last_log);

    /* with nothing in flight the next one runs at once and its reply tells */
    fake.reset_stuck = false;
    CHECK_EQ(dc(RX0, CAN_DEVCTL_SET_TIMING, &t, sizeof(t), 0), 0);
    CHECK_EQ(xstats().restarts, restarts + 2);
    CHECK(fake.int_ena != 0, "output on");
    CHECK_EQ(lc.slept_ns, 0);
}

/* Shutdown (SIGTERM: the body returns) waits off libcan's loop, bounded. */
static void body_shutdown_stuck(void)
{
    write_frames(0x460, 2);
    fake.abort_slow = true;
}

static void body_shutdown_one_done(void)
{
    write_frames(0x470, 2);
    fake.txt[0] = TXT_TOK;  /* sent; its interrupt not handled before SIGTERM */
}

static void body_shutdown_pending(void)
{
    CAN_DEVCTL_TIMING t = T250;

    write_frames(0x480, 1);
    fake.abort_slow = true;
    CHECK_EQ(dc(RX0, CAN_DEVCTL_SET_TIMING, &t, sizeof(t), 0), 0);
    fake_advance_ms(5);
}

static void test_tx_wait(void)
{
    CHECK_EQ(run("", body_busoff_wait), 0);
    CHECK_EQ(run("-a", body_busoff_wait_auto), 0);
    CHECK_EQ(run("", body_restart_irq), 0);
    CHECK_EQ(run("", body_restart_deadline), 0);
    CHECK_EQ(run("", body_restart_abort), 0);
    CHECK_EQ(run("", body_restart_latest), 0);

    /* finish phase, abort phase, then given up: two phases of sleep at most */
    CHECK_EQ(run("", body_shutdown_stuck), 0);
    CHECK(lc.slept_ns >= 2ull * TX_QUIESCE_MS * 1000000u && lc.slept_ns <= 2ull * TX_QUIESCE_MS * 1000000u + 200000u,
          "slept %llu ns", (unsigned long long)lc.slept_ns);
    CHECK_EQ(g_drv.x.tx_abort_forced, 2);
    CHECK_EQ(g_drv.x.tx_aborted, 2);
    CHECK(!lc.timer_up && lc.timer_at_ms == 0, "timer gone before the wait");
    CHECK(strstr(lc.last_log, "stopped: tx") != NULL, "%s", lc.last_log);

    /* one frame went out, the other is aborted after the finish phase */
    CHECK_EQ(run("", body_shutdown_one_done), 0);
    CHECK(lc.slept_ns >= 1ull * TX_QUIESCE_MS * 1000000u && lc.slept_ns <= 1ull * TX_QUIESCE_MS * 1000000u + 200000u,
          "slept %llu ns", (unsigned long long)lc.slept_ns);
    CHECK_EQ(g_drv.x.tx_ok, 1);
    CHECK_EQ(g_drv.x.tx_aborted, 1);
    CHECK_EQ(g_drv.x.tx_abort_forced, 0);

    /* a wait already on keeps its deadline; the pending restart is dropped */
    CHECK_EQ(run("", body_shutdown_pending), 0);
    CHECK(lc.slept_ns >= (2ull * TX_QUIESCE_MS - 5) * 1000000u &&
          lc.slept_ns <= (2ull * TX_QUIESCE_MS - 5) * 1000000u + 200000u, "slept %llu ns",
          (unsigned long long)lc.slept_ns);
    CHECK_EQ(g_drv.x.restarts, 0);
    CHECK_EQ(g_drv.x.tx_abort_forced, 1);
}

/* ---- time across the 32-bit wrap ----------------------------------------- */

#define W32_US 0x100000000ull

/* Frame timestamps are 32-bit microseconds and wrap every 2^32 us (about
 * 71.6 minutes). Deadlines and intervals in the driver are 64-bit, so a TX
 * wait across the wrap ends on time, and the 64-bit timestamp goes on. */
static void body_wrap(void)
{
    CAN_DEVCTL_TIMING t = T250;
    uint64_t a = 0, b = 0;
    uint32_t ts = 0;

    lc.now_ns = (W32_US - 2000u) * 1000u;   /* 2 ms before the wrap; offset 0 */
    uint64_t start_us = lc.now_ns / 1000u;
    fake_block_readers(RX0, 2);
    CHECK_EQ(dc(RX0, CTUCAN_DEVCTL_GET_TIMESTAMP64, &a, 0, sizeof(a)), sizeof(a));
    CHECK_EQ(a, start_us);
    rx_frame(0x123, 8);
    fake_pulse();
    CHECK_EQ(lc.nrx, 1);
    CHECK_EQ(lc.rx[0].msg.ext.timestamp, (uint32_t)(W32_US - 2000u));

    /* a restart wait whose deadline lies past the wrap */
    write_frames(0x490, 1);
    unsigned restarts = xstats().restarts;
    CHECK_EQ(dc(RX0, CAN_DEVCTL_SET_TIMING, &t, sizeof(t), 0), 0);
    CHECK_EQ(lc.timer_at_ms, start_us / 1000u + TX_QUIESCE_MS);
    CHECK(lc.timer_at_ms * 1000u > W32_US, "deadline past the wrap");
    fake_advance_ms(10);
    CHECK(lc.now_ns / 1000u > W32_US, "clock past the wrap");
    CHECK(!fake_timer_fire(), "not due 10 ms in, after the wrap");
    CHECK_EQ(xstats().state & RESTART_BIT, RESTART_BIT);
    CHECK(fake_timer_run(), "armed");
    ctucan_xstats_t x = xstats();
    CHECK_EQ(x.restarts, restarts + 1);
    CHECK_EQ(x.tx_aborted, 1);
    CHECK_EQ(x.state & (TXW_BIT | RESTART_BIT), 0);

    uint64_t elapsed = lc.now_ns / 1000u - start_us;
    rx_frame(0x124, 8);
    fake_pulse();
    CHECK_EQ(lc.nrx, 2);
    uint32_t ts2 = lc.rx[1].msg.ext.timestamp;
    CHECK(ts2 < 1000000u, "32-bit timestamp wrapped: %u", ts2);
    CHECK_EQ((uint32_t)(ts2 - lc.rx[0].msg.ext.timestamp), elapsed);
    CHECK_EQ(dc(RX0, CTUCAN_DEVCTL_GET_TIMESTAMP64, &b, 0, sizeof(b)), sizeof(b));
    CHECK_EQ(b - a, elapsed);
    CHECK(b > W32_US, "64-bit timestamp did not wrap");
    CHECK_EQ(dc(RX0, CAN_DEVCTL_GET_TIMESTAMP, &ts, 0, sizeof(ts)), sizeof(ts));
    CHECK_EQ(ts, (uint32_t)b);
    CHECK_EQ(lc.slept_ns, 0);
}

/* ---- slow TX diagnostics (docs/TESTS.md, restart_under_load) ------------- */

static void body_slow_tx(void)
{
    write_frames(0x500, 4);     /* the ring is full */
    fake.txt[0] = TXT_TOK;      /* sent at once, as on QEMU; its pulse is late */
    fake_advance_ms(1);
    write_frames(0x504, 1);     /* queues, and finds buffer 0 done */
    fake_advance_ms(3);
    unsigned lines = lc.slog_lines;
    irq(INT_TXBHCI);
    fake_pulse();
    CHECK_EQ(lc.slog_lines - lines, 1);
    CHECK(strstr(lc.last_log, "slow TX: frame 0 buf 0 st 4: 4000 us to retire (pulse); pulses since 1, "
                              "prev gap 0 us; final seen 3000 us before; inflight 4 queued 1; slow 1 unlogged 0") !=
          NULL, "%s", lc.last_log);

    /* another slow frame within SLOW_LOG_MS: counted, not logged */
    lines = lc.slog_lines;
    fake.txt[1] = TXT_TOK;
    irq(INT_TXBHCI);
    fake_pulse();
    CHECK_EQ(lc.slog_lines, lines);
    CHECK_EQ(g_drv.tx_slow, 2);

    fake_advance_ms(SLOW_LOG_MS);
    fake.txt[2] = TXT_TOK;
    irq(INT_TXBHCI);
    fake_pulse();
    CHECK_EQ(lc.slog_lines - lines, 1);
    CHECK(strstr(lc.last_log, "frame 2 buf 2 st 4: 104000 us to retire (pulse); pulses since 3, prev gap 100000 us; "
                              "final seen 0 us before; inflight 3 queued 0; slow 3 unlogged 1") != NULL, "%s",
          lc.last_log);

    lines = lc.slog_lines;
    fake.txt[3] = TXT_TOK;
    fake.txt[0] = TXT_TOK;  /* frame 4, set ready in the pulse that retired buffer 0 */
    irq(INT_TXBHCI);
    fake_pulse();
    CHECK_EQ(g_drv.tx_slow, 5);     /* frame 3 is slow, frame 4 (in buffer 0 for 100 ms) too */
    CHECK_EQ(lc.slog_lines, lines); /* within SLOW_LOG_MS of the last line */

    /* a frame retired in time logs nothing and counts nothing */
    write_frames(0x510, 1);
    fake_advance_ms(1);
    fake.txt[1] = TXT_TOK;
    irq(INT_TXBHCI);
    fake_pulse();
    CHECK_EQ(g_drv.tx_slow, 5);
}

/* ---- a BAR that is not a CTU CAN FD core ---------------------------------- */

/* The registers of something else at those offsets mean nothing: the driver
 * must leave without writing one (INT_ENA_CLR, MODE and INT_STAT of the core
 * are other registers there). */
static void body_never(void)
{
    CHECK(false, "the driver started on a core that failed the identity check");
}

static void test_identify_fail(void)
{
    fake_device_id_force = 0x02020000u | 0xbeefu;
    int rc = run("", body_never);
    fake_device_id_force = 0;
    CHECK_EQ(rc, ENODEV);
    CHECK(fake.nlog == 0, "%u register write(s) to a core that failed the identity check, first at 0x%x",
          fake.nlog, fake.nlog > 0 ? fake.log[0].off : 0u);
}

int main(void)
{
    test_identify_fail();
    test_start_stop();
    test_tx_ring();
    test_tx_wrap();
    test_busoff();
    test_error_states();
    test_service();
    test_reports();
    test_retr_limit();
    test_devctl();
    test_tx_wait();
    CHECK_EQ(run("", body_wrap), 0);
    CHECK_EQ(run("", body_slow_tx), 0);
    return check_exit("test_driver");
}
