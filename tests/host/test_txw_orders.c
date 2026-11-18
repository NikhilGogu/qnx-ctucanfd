/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The TX wait (docs/DESIGN.md, section 5) under every order of events, a few
 * events deep. The driver is included whole, as in test_driver.c, and runs
 * on the fake core and the fake libcan. For each start state and each
 * sequence of DEPTH events from the alphabet below, the driver is started
 * afresh, brought to the start state with events from the same alphabet,
 * driven through the sequence, then drained. The invariants are checked
 * after every event (and after every millisecond of virtual time), and once
 * more after the drain. No randomness: every sequence is enumerated.
 *
 * Alphabet, what each event does:
 *   SEND     the core sends one frame: the ready TXT buffer with the highest
 *            TX_PRIORITY (lowest index on a tie) becomes TOK, its id goes to
 *            the wire log and TXBHCI is raised. Nothing is sent while
 *            FAULT_STATE says bus-off. No interrupt pulse yet.
 *   PULSE    one interrupt pulse as libcan handles it (fake_pulse()).
 *   ABTDONE  aborts in progress finish: every TXT_ABTP buffer becomes
 *            TXT_ABT and TXBHCI is raised. No pulse yet.
 *   WRITE    a client writes one frame to tx1 (transmit() callback).
 *   DEVCTL   a plain devctl (GET_XSTATS), which continues a wait.
 *   TIMING   SET_TIMING (a core restart, deferred while frames are in
 *            flight or a wait is on).
 *   BUSOFF   FAULT_STATE goes bus-off with FCSI, then a pulse.
 *   RECOVER  FAULT_STATE goes error active with FCSI, then a pulse.
 *   ADV5     5 ms of virtual time in 1 ms ticks; the wait timer fires at its
 *            time (fake_timer_fire() after every tick), as on QNX.
 *   ADV20    the same for 20 ms.
 *   LATE25   25 ms pass and the timer does not fire: its helper thread is
 *            late. The wait now has no time bound (invariant d skips it).
 *   FIRE     the late timer delivers its devctl, if it is due.
 * With abort_slow set in the start state an abort leaves the buffer
 * TXT_ABTP until ABTDONE; a sequence without ABTDONE is an abort that never
 * completes. Without it aborts finish at once, as on QEMU.
 *
 * Not modelled: RX traffic and reader wake-ups; TXT_ERR, TXT_PER and lost
 * buffers; a frame that is mid-transmission when aborted and still ends TOK
 * (the core goes straight from ready to TOK); the error warning and error
 * passive states; a soft reset that never completes; interrupt storms;
 * posted writes; more than one TX mailbox; shutdown (test_driver.c covers it).
 *
 * Invariants:
 *   a  every frame written is counted exactly once: queued + in flight +
 *      tx_ok + tx_aborted (with tx_abort_forced) + tx_failed + tx_lost equals
 *      the frames written, tx_failed and tx_lost stay 0, tx_ok never exceeds
 *      what went on the wire, and after the drain it equals it.
 *   b  frames reach the wire in the order they were written, each once, and
 *      the driver never sees an order break (tx_order_err stays 0).
 *   c  the virtual sleep total stays 0: libcan's thread never sleeps.
 *   d  the wait always ends. While the timer has never been late in this
 *      wait, it lasts at most 2 * TX_QUIESCE_MS (40 ms) from its start: the
 *      FINISH phase ends at the first deadline and the ABORT phase gets one
 *      more TX_QUIESCE_MS. After a bus-off it ends at most TX_QUIESCE_MS
 *      (20 ms) after the last bus-off, which starts the ABORT phase afresh
 *      (DESIGN.md sections 5 and 8). The bound is checked at every tick of
 *      virtual time while the wait is on and when it ends. A buffer given up
 *      had its abort for at least TX_QUIESCE_MS first, and none is given up
 *      while still ready (never aborted). In the drain the timer is on time
 *      and the wait ends within 40 ms of the drain's start.
 *   e  a TXT buffer is set ready (TXCR) only while no wait is on, there is
 *      no bus-off hold and the core is started. With no wait and no hold,
 *      queued frames are not left waiting while the head buffer is free,
 *      unless an enabled interrupt is pending to bring the driver back.
 *
 * The drain after each sequence: 50 ms with the timer on time, then a pulse.
 * The wait must be over, and if one was on, every frame that was in a TXT
 * buffer at the drain's start is retired. Then the bus works again (error
 * active, aborts finish, every ready buffer sent, pulses) until nothing is
 * queued or in flight, and the counts must close: tx_ok equals the frames
 * on the wire and tx_ok + tx_aborted equals the frames written.
 *   f  a restart SET_TIMING asked for runs exactly once: at once if
 *      nothing is in flight, otherwise when the wait ends. It is pending only
 *      while a wait is on, the core reset happens with no wait on and no
 *      frame in a TXT buffer, and never inside an interrupt batch: when a
 *      reset is written during a pulse, the driver's last INT_STAT read
 *      showed nothing pending, so service() had left its loop. (With no RX
 *      and no interrupt storm in the model, a service() pass runs only
 *      after an INT_STAT read with something pending.)
 *   g  the timer is armed exactly when a wait is on, for the phase's end.
 */
#include <inttypes.h>
#include "driver_harness.h"

enum {
    E_SEND, E_PULSE, E_ABTDONE, E_WRITE, E_DEVCTL, E_TIMING, E_BUSOFF, E_RECOVER,
    E_ADV5, E_ADV20, E_LATE25, E_FIRE, NEV
};

static const char *const ev_name[NEV] = {
    "SEND", "PULSE", "ABTDONE", "WRITE", "DEVCTL", "TIMING", "BUSOFF", "RECOVER",
    "ADV5", "ADV20", "LATE25", "FIRE",
};

#define DEPTH       4u
#define MAX_PREFIX  8u
#define WIRE_MAX    64u
#define WAIT_BOUND  (2u * TX_QUIESCE_MS)

typedef struct {
    const char *name;
    const char *args;
    bool        slow;               /* fake.abort_slow */
    unsigned    n;
    int         ev[MAX_PREFIX];
} start_t;

/* Each start state is reached through the alphabet itself, with every
 * invariant checked on the way. */
static const start_t starts[] = {
    { "idle 1 in flight",            "",   false, 1, { E_WRITE } },
    { "idle 1 in flight, slow abort", "",  true,  1, { E_WRITE } },
    { "idle 2 in flight",            "",   false, 2, { E_WRITE, E_WRITE } },
    { "idle 2 in flight, slow abort", "",  true,  2, { E_WRITE, E_WRITE } },
    { "idle 3 in flight",            "",   false, 3, { E_WRITE, E_WRITE, E_WRITE } },
    { "idle 3 in flight, slow abort", "",  true,  3, { E_WRITE, E_WRITE, E_WRITE } },
    { "idle 4 in flight + 2 queued", "",   false, 6, { E_WRITE, E_WRITE, E_WRITE, E_WRITE, E_WRITE, E_WRITE } },
    { "idle 4 in flight + 2 queued, slow abort", "", true, 6,
      { E_WRITE, E_WRITE, E_WRITE, E_WRITE, E_WRITE, E_WRITE } },
    { "FINISH, restart pending",     "",   false, 3, { E_WRITE, E_WRITE, E_TIMING } },
    { "FINISH, restart pending, slow abort", "", true, 3, { E_WRITE, E_WRITE, E_TIMING } },
    { "ABORT, restart pending, aborts stuck", "", true, 4, { E_WRITE, E_WRITE, E_TIMING, E_ADV20 } },
    { "ABORT after bus-off",         "",   false, 3, { E_WRITE, E_WRITE, E_BUSOFF } },
    { "ABORT after bus-off, aborts stuck", "", true, 3, { E_WRITE, E_WRITE, E_BUSOFF } },
    { "ABORT after bus-off, -a",     "-a", false, 3, { E_WRITE, E_WRITE, E_BUSOFF } },
    { "ABORT after bus-off, -a, aborts stuck", "-a", true, 3, { E_WRITE, E_WRITE, E_BUSOFF } },
};
#define NSTART (sizeof(starts) / sizeof(starts[0]))

static const CAN_DEVCTL_TIMING T250 = { .bit_rate_prescaler = 8, .time_segment_1 = 43, .time_segment_2 = 6,
                                        .sync_jump_width = 6 };

/* ---- the test's model of what happened ---------------------------------- */

static struct {
    const start_t *st;
    int      seq[DEPTH];
    unsigned written;               /* frames the client wrote, all accepted */
    uint32_t next_id;
    uint32_t wire[WIRE_MAX];        /* ids the core sent, in order */
    unsigned nwire;
    unsigned restarts0, restarts;   /* g_drv.x.restarts at the start, and owed since */
    unsigned resets;                /* core resets seen while the body ran */
    bool     restart_owed;          /* SET_TIMING deferred, not yet run */
    bool     in_timing, timing_reset;   /* inside SET_TIMING; it reset the core at once */
    bool     wait_on, late;         /* a wait as the test saw it; its timer was late */
    uint64_t anchor_ms;             /* its start or its last bus-off */
    bool     after_busoff;          /* the anchor is a bus-off: ABORT phase only */
    uint32_t busoffs;               /* stats.bus_off_state_count last seen */
    bool     in_batch;              /* the driver's last INT_STAT read in this pulse showed work */
    uint64_t abort_ms[CTUCAN_TXT_BUFFERS];  /* when the driver last aborted a ready buffer */
    uint32_t forced;                /* x.tx_abort_forced last seen */
} m;

/* A TXT buffer the core may still send from must be one the driver has in
 * flight; otherwise a frame given up could still reach the wire. */
static void check_owned(const char *when)
{
    for (unsigned b = 0; b < CTUCAN_TXT_BUFFERS; b++) {
        bool mine = b < g_drv.ntxb && (b + g_drv.ntxb - g_drv.txb_tail) % g_drv.ntxb < g_drv.txb_n;
        CHECK(mine || (fake.txt[b] != TXT_RDY && fake.txt[b] != TXT_TRAN),
              "(a) %s: buffer %u is ready but not in flight", when, b);
    }
}

static void on_rd(uint32_t off, uint32_t val)
{
    if (off == CTUCAN_INT_STAT && g_drv.in_event) {
        m.in_batch = (val & g_drv.int_ena & ~INT_RBNEI) != 0;
    }
}

static void on_wr(uint32_t off, uint32_t val)
{
    if (off == CTUCAN_TX_COMMAND && (val & TXC_TXCR)) {
        CHECK(g_drv.txw == TXW_IDLE && !g_drv.busoff_hold && g_drv.hw_started,
              "(e) TXCR 0x%x with txw %d hold %d started %d", val, (int)g_drv.txw, g_drv.busoff_hold,
              g_drv.hw_started);
    }
    if (off == CTUCAN_TX_COMMAND && (val & TXC_TXCA)) {
        for (unsigned b = 0; b < CTUCAN_TXT_BUFFERS; b++) {
            if ((val & TXC_TXB(b)) && (fake.txt[b] == TXT_RDY || fake.txt[b] == TXT_TRAN)) {
                m.abort_ms[b] = fake_now_ms();
            }
        }
    }
    if (off == CTUCAN_MODE && (val & MODE_RST)) {
        /* every reset is one that was asked for: the deferred one, or the
         * one a SET_TIMING runs at once (its own txw_poll() may end the
         * wait and run the deferred one first) */
        m.resets++;
        if (m.restart_owed) {
            m.restart_owed = false;
            m.restarts++;
        } else if (m.in_timing && !m.timing_reset) {
            m.timing_reset = true;
            m.restarts++;
        } else {
            CHECK(false, "(f) core reset nobody asked for");
        }
        CHECK(!g_drv.restart_pending, "(f) core reset with the restart still pending");
        CHECK(!(g_drv.in_event && m.in_batch), "(f) core reset inside an interrupt batch");
        CHECK(g_drv.txw == TXW_IDLE, "(f) core reset with the wait on (txw %d)", (int)g_drv.txw);
        CHECK(g_drv.txb_n == 0, "(f) core reset with %u frame(s) in TXT buffers", g_drv.txb_n);
        check_owned("at the core reset");
    }
}

static unsigned queued(void)
{
    return lc.dev[TX1]->msg_queue->msgcnt;
}

/* d: FINISH then ABORT from the wait's start; ABORT alone from a bus-off */
static uint64_t bound(void)
{
    return m.after_busoff ? TX_QUIESCE_MS : WAIT_BOUND;
}

/* A bus-off seen by the driver (it counts each one): from here the frames
 * get the ABORT phase alone, whatever phase the wait was in. */
static void note_busoff(uint64_t now)
{
    if (g_drv.stats.bus_off_state_count != m.busoffs) {
        m.busoffs = g_drv.stats.bus_off_state_count;
        if (m.wait_on) {
            m.anchor_ms = now;
            m.after_busoff = true;
        }
    }
}

/* The wait's start and end, seen where they happen: txw_arm() arms the
 * timer for a phase, txw_done() (and nothing else while the driver runs)
 * disarms it. One devctl can end a wait and start the next. */
static void on_timer_arm(uint64_t at_ms)
{
    uint64_t now = fake_now_ms();

    if (at_ms == 0) {
        /* d: what the wait gave up (still aborting) had its abort for at
         * least TX_QUIESCE_MS first; a buffer still ready was never aborted */
        if (g_drv.x.tx_abort_forced != m.forced) {
            m.forced = g_drv.x.tx_abort_forced;
            for (unsigned b = 0; b < CTUCAN_TXT_BUFFERS; b++) {
                if (fake.txt[b] == TXT_ABTP) {
                    CHECK(now - m.abort_ms[b] >= TX_QUIESCE_MS, "(d) buffer %u given up %" PRIu64
                          " ms after its abort", b, now - m.abort_ms[b]);
                }
            }
            check_owned("at a give-up");
        }
        CHECK(g_drv.txw == TXW_IDLE, "(g) timer disarmed with the wait on (txw %d)", (int)g_drv.txw);
        CHECK(m.wait_on, "(g) timer disarmed with no wait on");
        if (m.wait_on && !m.late) {
            CHECK(now - m.anchor_ms <= bound(), "(d) a wait ended %" PRIu64 " ms after its %s, timer on time",
                  now - m.anchor_ms, m.after_busoff ? "bus-off" : "start");
        }
        m.wait_on = false;
        return;
    }
    CHECK(g_drv.txw != TXW_IDLE, "(g) timer armed with no wait on");
    CHECK_EQ(at_ms, now + TX_QUIESCE_MS);
    if (!m.wait_on) {
        m.wait_on = true;
        m.late = false;
        m.anchor_ms = now;
        m.after_busoff = false;
    }
    note_busoff(now);
}

/* The bound (d) on the wait that is on; restart bookkeeping (f). Runs after
 * every event and after every tick of virtual time. */
static void track(void)
{
    uint64_t now = fake_now_ms();
    bool on = g_drv.txw != TXW_IDLE;

    CHECK_EQ(m.wait_on, on);
    note_busoff(now);
    if (m.wait_on && !m.late) {
        CHECK(now - m.anchor_ms <= bound(), "(d) wait on %" PRIu64 " ms after its %s, timer on time",
              now - m.anchor_ms, m.after_busoff ? "bus-off" : "start");
    }
    CHECK(!m.restart_owed || g_drv.restart_pending, "(f) restart dropped without running");
    CHECK(!g_drv.restart_pending || on, "(f) restart pending with no wait to end");
    CHECK_EQ(g_drv.x.restarts, m.restarts0 + m.restarts);
    CHECK_EQ(m.resets, m.restarts);
}

static void check_all(void)
{
    const ctucan_xstats_t *x = &g_drv.x;
    unsigned infl = g_drv.txb_n, q = queued();

    track();

    /* a */
    CHECK_EQ(x->tx_started + q, m.written);
    CHECK_EQ(x->tx_ok + x->tx_aborted + x->tx_failed + x->tx_lost + infl, x->tx_started);
    CHECK_EQ(x->tx_failed, 0);
    CHECK_EQ(x->tx_lost, 0);
    CHECK(x->tx_abort_forced <= x->tx_aborted, "(a) forced %u > aborted %u", x->tx_abort_forced, x->tx_aborted);
    CHECK(x->tx_ok <= m.nwire, "(a) tx_ok %u, %u on the wire", x->tx_ok, m.nwire);
    CHECK(m.nwire - x->tx_ok <= infl, "(a) %u sent, %u counted, %u in flight", m.nwire, x->tx_ok, infl);
    CHECK(infl <= g_drv.ntxb && (g_drv.txb_tail + infl) % g_drv.ntxb == g_drv.txb_head,
          "(a) ring tail %u n %u head %u", g_drv.txb_tail, infl, g_drv.txb_head);
    for (unsigned k = 0; k < infl; k++) {
        unsigned b = (g_drv.txb_tail + k) % g_drv.ntxb;
        CHECK(fake.txt[b] != TXT_ETY, "(a) buffer %u in flight but empty", b);
    }
    check_owned("after an event");

    /* b */
    CHECK_EQ(x->tx_order_err, 0);

    /* c */
    CHECK_EQ(lc.slept_ns, 0);

    /* e: with no wait and no hold, nothing queued sits behind a free buffer
     * unless an enabled interrupt is pending to bring the driver back (a
     * buffer freed by the core, before its pulse) */
    if (g_drv.txw == TXW_IDLE && !g_drv.busoff_hold && g_drv.hw_started && q > 0) {
        CHECK(infl == g_drv.ntxb || !ctucan_txt_writable(fake.txt[g_drv.txb_head]) ||
              (fake.mem[CTUCAN_INT_STAT / 4] & fake.int_ena) != 0,
              "(e) %u queued, %u in flight, head buffer %u state %u, no interrupt pending", q, infl,
              g_drv.txb_head, fake.txt[g_drv.txb_head]);
    }

    /* g */
    CHECK_EQ(lc.timer_at_ms != 0, g_drv.txw != TXW_IDLE);
    if (lc.timer_at_ms != 0) {
        CHECK_EQ(lc.timer_at_ms, g_drv.txw_end_ms);
    }
}

/* ---- events --------------------------------------------------------------- */

static void ev_send(void)
{
    if (fake.mem[CTUCAN_EWL_ERP_FAULT / 4] & FS_BOF) {
        return;     /* a bus-off node does not transmit */
    }
    uint32_t prio = fake.mem[CTUCAN_TX_PRIORITY / 4];
    int best = -1;
    unsigned best_p = 0;

    for (unsigned b = 0; b < CTUCAN_TXT_BUFFERS; b++) {
        unsigned p = (prio >> (4u * b)) & 7u;
        if (fake.txt[b] == TXT_RDY && (best < 0 || p > best_p)) {
            best = (int)b;
            best_p = p;
        }
    }
    if (best < 0) {
        return;
    }
    uint32_t id = txt_id((unsigned)best);
    CHECK(m.nwire == 0 || id > m.wire[m.nwire - 1], "(b) 0x%x on the wire after 0x%x", id,
          m.nwire > 0 ? m.wire[m.nwire - 1] : 0);
    CHECK(id < m.next_id, "(b) 0x%x was never written", id);
    if (m.nwire < WIRE_MAX) {
        m.wire[m.nwire++] = id;
    }
    fake.txt[best] = TXT_TOK;
    irq(INT_TXBHCI);
}

static void ev_abtdone(void)
{
    bool any = false;

    for (unsigned b = 0; b < CTUCAN_TXT_BUFFERS; b++) {
        if (fake.txt[b] == TXT_ABTP) {
            fake.txt[b] = TXT_ABT;
            any = true;
        }
    }
    if (any) {
        irq(INT_TXBHCI);
    }
}

static void ev_write(void)
{
    CAN_MSG f = frame(m.next_id++, 8);
    CHECK_EQ(fake_write(TX1, &f), 0);
    m.written++;
}

static void ev_timing(void)
{
    CAN_DEVCTL_TIMING t = T250;

    m.in_timing = true;
    m.timing_reset = false;
    CHECK_EQ(dc(RX0, CAN_DEVCTL_SET_TIMING, &t, sizeof(t), 0), 0);
    m.in_timing = false;
    if (g_drv.restart_pending) {
        CHECK(!m.timing_reset, "(f) SET_TIMING both ran and deferred its restart");
        m.restart_owed = true;  /* deferred: runs when the wait ends */
    } else {
        CHECK(m.timing_reset, "(f) SET_TIMING neither ran nor deferred its restart");
    }
}

static void advance(unsigned ms)
{
    for (unsigned i = 0; i < ms; i++) {
        fake_advance_ms(1);
        (void)fake_timer_fire();
        check_all();
    }
}

static void event(int e)
{
    switch (e) {
    case E_SEND:    ev_send(); break;
    case E_PULSE:   fake_pulse(); break;
    case E_ABTDONE: ev_abtdone(); break;
    case E_WRITE:   ev_write(); break;
    case E_DEVCTL: {
        ctucan_xstats_t x;
        CHECK_EQ(dc(RX0, CTUCAN_DEVCTL_GET_XSTATS, &x, 0, sizeof(x)), sizeof(x));
        break;
    }
    case E_TIMING:  ev_timing(); break;
    case E_BUSOFF:  set_fault(FS_BOF, 256); fake_pulse(); break;
    case E_RECOVER: set_fault(FS_ERA, 0); fake_pulse(); break;
    case E_ADV5:    advance(5); break;
    case E_ADV20:   advance(20); break;
    case E_LATE25:
        if (m.wait_on) {
            m.late = true;
        }
        fake_advance_ms(25);
        break;
    case E_FIRE:    (void)fake_timer_fire(); break;
    default: break;
    }
    check_all();
}

/* ---- drain ---------------------------------------------------------------- */

static void drain(void)
{
    uint64_t t0 = fake_now_ms(), end = t0;
    uint32_t seq_hi = g_drv.tx_seq;     /* frames set ready before the drain have lower numbers */
    bool waiting = g_drv.txw != TXW_IDLE;

    /* the timer on time for 50 ms, then a pulse: the wait has ended */
    for (unsigned i = 0; i < 50; i++) {
        if (g_drv.txw != TXW_IDLE) {
            fake_advance_ms(1);
            (void)fake_timer_fire();
            end = fake_now_ms();
        } else {
            fake_advance_ms(1);
        }
        check_all();
    }
    fake_pulse();
    check_all();
    CHECK(g_drv.txw == TXW_IDLE, "(d) wait still on after the drain (txw %d)", (int)g_drv.txw);
    CHECK(end - t0 <= WAIT_BOUND, "(d) the drain's wait took %" PRIu64 " ms", end - t0);
    CHECK_EQ(lc.timer_at_ms, 0);
    CHECK(!g_drv.restart_pending && !m.restart_owed, "(f) restart still pending after the drain");
    /* a wait gives up what it waited for; with none, frames stay in flight
     * until the bus sends them, below */
    for (unsigned k = 0; waiting && k < g_drv.txb_n; k++) {
        unsigned b = (g_drv.txb_tail + k) % g_drv.ntxb;
        CHECK(g_drv.txs[b].seq >= seq_hi, "(d) frame %u from before the drain still in flight",
              g_drv.txs[b].seq);
    }

    /* then the bus works again until everything written is final */
    if (g_drv.busoff_hold || (fake.mem[CTUCAN_EWL_ERP_FAULT / 4] & FS_BOF)) {
        event(E_RECOVER);
    }
    for (unsigned r = 0; r < 16 && (g_drv.txb_n > 0 || queued() > 0); r++) {
        event(E_ABTDONE);
        for (unsigned b = 0; b < CTUCAN_TXT_BUFFERS; b++) {
            event(E_SEND);
        }
        event(E_PULSE);
    }
    const ctucan_xstats_t *x = &g_drv.x;
    CHECK_EQ(g_drv.txb_n, 0);
    CHECK_EQ(queued(), 0);
    CHECK(g_drv.txw == TXW_IDLE && lc.timer_at_ms == 0, "(g) idle and disarmed at the end");
    CHECK(tx_admit(&g_drv), "(e) TX admission open at the end");
    CHECK_EQ(x->tx_ok, m.nwire);
    CHECK_EQ(x->tx_ok + x->tx_aborted, m.written);
    CHECK_EQ(x->tx_started, m.written);
    for (unsigned i = 1; i < m.nwire; i++) {
        CHECK(m.wire[i] > m.wire[i - 1], "(b) wire order at %u", i);
    }
    CHECK_EQ(g_drv.x.restarts, m.restarts0 + m.restarts);
    CHECK_EQ(lc.slept_ns, 0);
}

/* ---- the run ---------------------------------------------------------------- */

static void body(void)
{
    memset(&m.written, 0, sizeof(m) - offsetof(typeof(m), written));
    m.next_id = 0x100;
    m.restarts0 = g_drv.x.restarts;
    m.busoffs = g_drv.stats.bus_off_state_count;
    m.forced = g_drv.x.tx_abort_forced;
    fake.abort_slow = m.st->slow;
    fake.on_rd = on_rd;
    fake.on_wr = on_wr;
    lc.on_timer_arm = on_timer_arm;
    check_all();
    for (unsigned i = 0; i < m.st->n; i++) {
        event(m.st->ev[i]);
    }
    for (unsigned i = 0; i < DEPTH; i++) {
        event(m.seq[i]);
    }
    drain();
    fake.on_rd = NULL;      /* shutdown is test_driver.c's */
    fake.on_wr = NULL;
    lc.on_timer_arm = NULL;
}

static void print_seq(void)
{
    fprintf(stderr, "  sequence: start \"%s\" (args \"%s\"), events", m.st->name, m.st->args);
    for (unsigned i = 0; i < DEPTH; i++) {
        fprintf(stderr, " %s", ev_name[m.seq[i]]);
    }
    fputc('\n', stderr);
}

int main(void)
{
    unsigned long nseq = 0, nbad = 0;
    unsigned total = 1;

    for (unsigned i = 0; i < DEPTH; i++) {
        total *= NEV;
    }
    for (unsigned s = 0; s < NSTART; s++) {
        m.st = &starts[s];
        for (unsigned code = 0; code < total; code++) {
            unsigned c = code, failed = check_failed;
            for (unsigned i = 0; i < DEPTH; i++) {
                m.seq[i] = (int)(c % NEV);
                c /= NEV;
            }
            CHECK_EQ(run(m.st->args, body), 0);
            nseq++;
            if (check_failed != failed && nbad++ < 10) {
                print_seq();
            }
        }
    }
    printf("test_txw_orders: %lu sequences (depth %u, %u events, %zu start states), %u checks, %u failed\n", nseq,
           DEPTH, (unsigned)NEV, NSTART, check_run, check_failed);
    if (nbad > 0) {
        fprintf(stderr, "test_txw_orders: %lu failing sequence(s)\n", nbad);
    }
    return check_failed == 0 ? 0 : 1;
}
