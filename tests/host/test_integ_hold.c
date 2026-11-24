/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The integration hold: after SETTINGS.ENA the core is bus-off while it
 * integrates into the bus, and a Set ready command in that time sends the
 * TXT buffer to "Aborted" (datasheet 2.9.7, note). The driver keeps TBFBO = 1,
 * so it must not issue Set ready while FAULT_STATE reads bus-off after ENA,
 * and may again once it reads error active.
 *
 * The fake core models that with fake_integrate (tests/host/fake_core.h):
 * FAULT_STATE reads bus-off after ENA until a case writes error active, and
 * a Set ready while it reads bus-off ends in TXT_ABT. QEMU 6.2 has no
 * integration phase: it reads FAULT_STATE as 0, none of error active, error
 * passive and bus-off (docs/DESIGN.md, section 1). The hold must not be set
 * for that reading, or TX would never start on QEMU. These cases show the
 * driver against the fake core only.
 *
 * Cases: (a) held at start, (b) released by FCSI, (c) released by polling,
 * (d) the same after a SET_TIMING restart, (e) FAULT_STATE reads 0 (QEMU),
 * error active or error passive at start and after a restart: nothing is
 * held and the frame is sent. The driver is reached only through libcan's callbacks and the
 * devctls, and GET_XSTATS state bit 12 (the hold), so the file also builds
 * against the driver before the hold existed, for the comparison in
 * results/fix-integration-hold.md:
 *   make integ_hold_base BASE=/path/to/other/src
 * (that build defines BASE_DRIVER, which leaves out the one check of a field).
 */
#include "driver_harness.h"

#define INTEG_BIT (1u << 12)    /* GET_XSTATS state: TX held while the core integrates */

static const CAN_DEVCTL_TIMING T250 = { .bit_rate_prescaler = 8, .time_segment_1 = 43, .time_segment_2 = 6,
                                        .sync_jump_width = 6 };

static bool integ_hold(void)
{
    return (xstats().state & INTEG_BIT) != 0;
}

static void write_one(unsigned id)
{
    CAN_MSG m = frame(id, 8);

    CHECK_EQ(fake_write(TX1, &m), 0);
}

static unsigned tx_cmds(void)
{
    return fake_write_count(CTUCAN_TX_COMMAND);
}

static unsigned ready_buffers(void)
{
    unsigned n = 0;

    for (unsigned b = 0; b < CTUCAN_TXT_BUFFERS; b++) {
        n += fake.txt[b] == TXT_RDY;
    }
    return n;
}

static unsigned aborted_buffers(void)
{
    unsigned n = 0;

    for (unsigned b = 0; b < CTUCAN_TXT_BUFFERS; b++) {
        n += fake.txt[b] == TXT_ABT;
    }
    return n;
}

static unsigned fault_reads;

static void count_fault_reads(uint32_t off, uint32_t val)
{
    (void)val;
    fault_reads += off == CTUCAN_EWL_ERP_FAULT;
}

/* The core ends integration: error active, and FCSI when the case says so. */
static void integration_over(bool fcsi)
{
    if (fcsi) {
        set_fault(FS_ERA, 0);
    } else {
        fake.mem[CTUCAN_EWL_ERP_FAULT / 4] = FS_ERA | 96u;
    }
}

static void check_nothing_sent(const char *what)
{
    CHECK_EQ(tx_cmds(), 0);
    CHECK_EQ(ready_buffers(), 0);
    CHECK_EQ(aborted_buffers(), 0);
    CHECK(lc.dev[TX1]->msg_queue->msgcnt == 1, "%s: the frame is not queued (%u)", what,
          (unsigned)lc.dev[TX1]->msg_queue->msgcnt);
}

static void check_no_busoff(const char *what)
{
    ctucan_xstats_t x = xstats();

    CHECK(stats().bus_off_state_count == 0, "%s: counted as a bus-off", what);
    CHECK(!busoff_hold(), "%s: busoff_hold set", what);
    CHECK(x.busoff_recovered == 0, "%s: counted as a bus-off recovery", what);
    CHECK(x.tx_aborted == 0 && x.tx_failed == 0, "%s: a frame ended aborted or failed", what);
}

/* (a) and (b): held at start, released by FCSI. */
static void body_fcsi(void)
{
    CHECK(integ_hold(), "hold set at start while FAULT_STATE reads bus-off");
#ifndef BASE_DRIVER
    CHECK_EQ(g_drv.integ_holds, 1);
#endif
    write_one(0x300);
    check_nothing_sent("held at start");
    check_no_busoff("held at start");
    /* an event that is not a FAULT_STATE change leaves it held */
    irq(INT_EWLI);
    fake_pulse();
    CHECK(integ_hold(), "hold kept by an unrelated event");
    check_nothing_sent("after an unrelated event");

    integration_over(true);
    fake_pulse();
    CHECK(!integ_hold(), "hold released by FCSI with error active");
    CHECK_EQ(tx_cmds(), 1);
    CHECK_EQ(ready_buffers(), 1);
    CHECK_EQ(aborted_buffers(), 0);
    CHECK_EQ(txt_id(0), 0x300);
    CHECK_EQ(lc.dev[TX1]->msg_queue->msgcnt, 0);
    check_no_busoff("released by FCSI");
    CHECK_EQ(xstats().tx_started, 1);
#ifndef BASE_DRIVER
    CHECK_EQ(g_drv.integ_holds, 1);
#endif
}

/* (c): released by polling, no FCSI. */
static void body_poll(void)
{
    CHECK(integ_hold(), "hold set at start");
    write_one(0x310);
    check_nothing_sent("held at start");

    /* still bus-off: one more write is one FAULT_STATE read and stays held */
    fault_reads = 0;
    fake.on_rd = count_fault_reads;
    write_one(0x311);
    fake.on_rd = NULL;
    CHECK_EQ(fault_reads, 1);
    CHECK(integ_hold(), "hold kept while FAULT_STATE reads bus-off");
    CHECK_EQ(tx_cmds(), 0);
    CHECK_EQ(lc.dev[TX1]->msg_queue->msgcnt, 2);

    /* integration ends without FCSI: the next write finds it */
    integration_over(false);
    CHECK_EQ(fake.mem[CTUCAN_INT_STAT / 4] & INT_FCSI, 0);
    write_one(0x312);
    CHECK(!integ_hold(), "hold released by the poll");
    CHECK_EQ(lc.dev[TX1]->msg_queue->msgcnt, 0);
    CHECK_EQ(ready_buffers(), 3);
    CHECK_EQ(aborted_buffers(), 0);
    CHECK_EQ(txt_id(0), 0x310);
    CHECK_EQ(txt_id(1), 0x311);
    CHECK_EQ(txt_id(2), 0x312);
    check_no_busoff("released by the poll");

    /* the poll keeps the driver's view of FAULT_STATE: a real bus-off after
     * it is still a bus-off */
    set_fault(FS_BOF, 256);
    fake_pulse();
    CHECK_EQ(stats().bus_off_state_count, 1);
    CHECK(busoff_hold(), "bus-off after the poll is seen");
    CHECK(!integ_hold(), "the integration hold stays off");
}

/* (d1): a SET_TIMING restart with nothing in flight. */
static void body_restart(void)
{
    CAN_DEVCTL_TIMING t = T250;

    CHECK(integ_hold(), "hold set at start");
    integration_over(true);
    fake_pulse();
    CHECK(!integ_hold(), "released at start");
    write_one(0x320);
    CHECK_EQ(ready_buffers(), 1);
    fake.txt[0] = TXT_TOK;      /* the frame is done */
    irq(INT_TXBHCI);
    fake_pulse();
    CHECK_EQ(xstats().tx_ok, 1);

    fake.nlog = 0;
    CHECK_EQ(dc(RX0, CAN_DEVCTL_SET_TIMING, &t, sizeof(t), 0), 0);
    CHECK_EQ(xstats().restarts, 1);
    CHECK(integ_hold(), "hold set again after the restart");
    write_one(0x321);
    check_nothing_sent("after the restart");
    check_no_busoff("after the restart");

    /* ends by FCSI */
    integration_over(true);
    fake_pulse();
    CHECK(!integ_hold(), "hold released by FCSI after the restart");
    CHECK_EQ(tx_cmds(), 1);
    CHECK_EQ(ready_buffers(), 1);
    CHECK_EQ(aborted_buffers(), 0);
    CHECK_EQ(txt_id(0), 0x321);
    check_no_busoff("after the restart and FCSI");

    /* a second restart, ended by the poll */
    fake.txt[0] = TXT_TOK;      /* the frame is done */
    irq(INT_TXBHCI);
    fake_pulse();
    CHECK_EQ(xstats().tx_ok, 2);
    fake.nlog = 0;
    CHECK_EQ(dc(RX0, CAN_DEVCTL_SET_TIMING, &t, sizeof(t), 0), 0);
    CHECK_EQ(xstats().restarts, 2);
    CHECK(integ_hold(), "hold set after the second restart");
    write_one(0x322);
    check_nothing_sent("after the second restart");
    integration_over(false);
    write_one(0x323);
    CHECK(!integ_hold(), "hold released by the poll after the restart");
    CHECK_EQ(lc.dev[TX1]->msg_queue->msgcnt, 0);
    CHECK_EQ(ready_buffers(), 2);
    CHECK_EQ(aborted_buffers(), 0);
    CHECK_EQ(txt_id(0), 0x322);
    CHECK_EQ(txt_id(1), 0x323);
    check_no_busoff("after the second restart");
}

/* (d2): the restart is deferred behind frames whose aborts never finish; it
 * runs from the TX wait, and the hold follows it. */
static void body_restart_deferred(void)
{
    CAN_DEVCTL_TIMING t = T250;

    integration_over(true);
    fake_pulse();
    CHECK(!integ_hold(), "released at start");
    write_one(0x330);
    write_one(0x331);
    fake.abort_slow = true;
    CHECK_EQ(dc(RX0, CAN_DEVCTL_SET_TIMING, &t, sizeof(t), 0), 0);
    CHECK((xstats().state & (1u << 11)) != 0, "restart pending");
    write_one(0x332);           /* queued during the wait */
    for (unsigned k = 0; k < 8 && fake_timer_run(); k++) {
    }
    CHECK_EQ(xstats().restarts, 1);
    CHECK(integ_hold(), "hold set after the deferred restart");
    fake.nlog = 0;
    fake_pulse();
    CHECK_EQ(tx_cmds(), 0);
    CHECK_EQ(ready_buffers(), 0);
    CHECK_EQ(aborted_buffers(), 0);
    CHECK_EQ(lc.dev[TX1]->msg_queue->msgcnt, 1);

    integration_over(true);
    fake_pulse();
    CHECK(!integ_hold(), "hold released after the deferred restart");
    CHECK_EQ(tx_cmds(), 1);
    CHECK_EQ(ready_buffers(), 1);
    CHECK_EQ(aborted_buffers(), 0);
    CHECK_EQ(lc.dev[TX1]->msg_queue->msgcnt, 0);
    CHECK_EQ(stats().bus_off_state_count, 0);
    CHECK_EQ(xstats().busoff_recovered, 0);
}

/* A bus-off recovery with -a is no restart and no integration: the hold
 * is not involved. */
static void body_recover(void)
{
    integration_over(true);
    fake_pulse();
    CHECK(!integ_hold(), "released at start");
    set_fault(FS_BOF, 256);
    fake_pulse();
    CHECK(busoff_hold(), "bus-off");
    CHECK(!integ_hold(), "no integration hold at bus-off");
    CHECK_EQ(ercrst_writes(), 1);
    set_fault(FS_ERA, 0);
    fake_pulse();
    CHECK(!busoff_hold() && !integ_hold(), "recovered");
    CHECK_EQ(xstats().busoff_recovered, 1);
}

/* (e): a reading other than bus-off at start holds nothing. The first run
 * is the QEMU case (FAULT_STATE reads 0); the others read error active and
 * error passive. The frame must be sent at once. */
static void body_nohold(void)
{
    CHECK(!integ_hold(), "no hold when FAULT_STATE does not read bus-off at start");
#ifndef BASE_DRIVER
    CHECK_EQ(g_drv.integ_holds, 0);
#endif
    write_one(0x340);
    CHECK_EQ(tx_cmds(), 1);
    CHECK_EQ(ready_buffers(), 1);
    CHECK_EQ(aborted_buffers(), 0);
    CHECK_EQ(lc.dev[TX1]->msg_queue->msgcnt, 0);
    check_no_busoff("no bus-off reading at start");
}

/* (e) again, with a restart: the same reading at the restart. */
static void body_nohold_restart(void)
{
    CAN_DEVCTL_TIMING t = T250;

    CHECK_EQ(dc(RX0, CAN_DEVCTL_SET_TIMING, &t, sizeof(t), 0), 0);
    CHECK_EQ(xstats().restarts, 1);
    CHECK(!integ_hold(), "no hold after a restart when FAULT_STATE does not read bus-off");
    fake.nlog = 0;
    write_one(0x350);
    CHECK_EQ(tx_cmds(), 1);
    CHECK_EQ(ready_buffers(), 1);
    CHECK_EQ(aborted_buffers(), 0);
}

int main(void)
{
    fake_integrate = true;
    CHECK_EQ(run("", body_fcsi), 0);
    CHECK_EQ(run("", body_poll), 0);
    CHECK_EQ(run("", body_restart), 0);
    CHECK_EQ(run("", body_restart_deferred), 0);
    CHECK_EQ(run("-a", body_recover), 0);
    fake_integrate = false;
    static const uint32_t reads[] = { 0, FS_ERA | 96u, FS_ERP | 96u };  /* QEMU 6.2, error active, error passive */
    for (unsigned i = 0; i < sizeof(reads) / sizeof(reads[0]); i++) {
        fake_fault_reset = reads[i];
        CHECK_EQ(run("", body_nohold), 0);
        CHECK_EQ(run("", body_nohold_restart), 0);
    }
    fake_fault_reset = 0;
    return check_exit("test_integ_hold");
}
