/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * dev-can-ctucanfd: QNX 8.0 CAN driver on the CAN DDK (libcan) for the CTU
 * CAN FD IP core on PCI, including the ctucan_pci board QEMU emulates.
 *
 * libcan's resource manager is single-threaded and runs every callback on
 * that thread, so the driver state needs no locks. can_resmgr_attach_intr()
 * associates the IRQ and a pulse code with libcan's pulse handler, which
 * calls event_handler(). Each interrupt pulse does a bounded batch of work
 * and ends with exactly one InterruptUnmask(). Work that is left keeps an
 * interrupt status bit pending in the core, so the core interrupts again
 * after the unmask and the next pulse takes the next batch
 * (docs/DESIGN.md, section 3).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/slog2.h>
#include <pci/pci.h>
#include <hw/libcan.h>
#include "ctucanfd_hw.h"
#include "ctucanfd_devctl.h"

#define DRV_NAME        "dev-can-ctucanfd"
#define MAX_RX_DEVS     16
#define MAX_TX_DEVS     16
#define PULSE_CODE_IRQ  (_PULSE_CODE_MINAVAIL + CTUCAN_PULSE_IRQ)
#define RX_BUDGET       64u     /* frames moved from the RX FIFO per pulse */
#define PASS_LIMIT      8u      /* INT_STAT passes per pulse */
#define REPORT_BUDGET   64u     /* RX mailbox returns to libcan per pulse */
#define TX_QUIESCE_MS   20u     /* wait for frames in flight, then as long again after an abort */
#define STUCK_LIMIT     8u      /* pulses in a row a mailbox may be returned without waking anyone */

#define container_of(p, type, member) ((type *)(void *)((char *)(p) - offsetof(type, member)))

_Static_assert(sizeof(ctucan_xstats_t) <= sizeof(CAN_DCMD_DATA),
               "private replies must fit where the standard CAN devctl replies go");

typedef struct drv drv_t;

typedef struct {
    CANDEV   cdev;
    drv_t   *drv;
    uint32_t mid;       /* TX: MID used by plain write(); RX: acceptance value */
    uint32_t mfilter;   /* RX: bits of mid that must match (0 = accept all) */
    bool     ext;       /* mid is a 29-bit identifier (-x), never inferred from its bits */
    bool     inited;
    unsigned stuck;     /* RX: pulses in a row in which a return woke nobody */
} mbox_t;

/* What the driver keeps about a frame in a TXT buffer. libcan's queue element
 * goes back to the free queue as soon as the frame is copied to hardware. */
typedef struct {
    uint32_t seq;
    uint32_t mid;
    uint8_t  len;
    bool     ext, rtr, late;
} txslot_t;

struct drv {
    /* options */
    unsigned pci_index, core, unit, nrx, ntx, msgq, waitq, ntxb;
    int      verbosity;
    bool     fd, brs, fd_always, autobus, ext;
    uint32_t clk_hz, bitrate, dbitrate;
    unsigned sp, dsp;

    /* hardware */
    pci_devhdl_t    pci;
    pci_irq_t       irq;
    int             iid;
    void           *map;
    ctucan_hw_t     hw;
    ctucan_hw_cfg_t cfg;
    uint32_t        int_ena;
    unsigned        ver_major, ver_minor, ncores;
    bool            resmgr_up, hw_started;

    /* interrupt service */
    bool        in_event, suppressed, notify_left;
    unsigned    budget, report_budget, reports_left, report_rr;
    uint32_t    rep_stuck;
    CANDEV     *last_rep;
    unsigned    last_msgcnt;
    size_t      last_waitcnt;

    /* TX */
    txslot_t txs[CTUCAN_TXT_BUFFERS];
    unsigned txb_head, txb_tail;    /* running counters; buffer = counter % ntxb */
    unsigned tx_rr;
    uint32_t tx_seq;
    bool     tx_stopped;            /* restart or shutdown in progress */

    /* errors */
    uint32_t fs;                    /* last FAULT_STATE bits */
    bool     ewl;                   /* last STATUS.EWL, tracked apart from FAULT_STATE */
    bool     busoff_hold;           /* set at bus-off, cleared only by FAULT_STATE = error active */

    int64_t          ts_delta;
    CAN_DEVCTL_STATS stats;
    ctucan_xstats_t  x;

    mbox_t rx[MAX_RX_DEVS];
    mbox_t tx[MAX_TX_DEVS];

#ifdef CTUCAN_FI
    bool fi_fixedprio, fi_nowake;
#endif
};

static drv_t g_drv;

static uint32_t now_us(const drv_t *d)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000 + d->ts_delta);
}

static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void fi_tick(drv_t *d);

/* ---- transmit ---------------------------------------------------------- */

static unsigned tx_inflight(const drv_t *d)
{
    return d->txb_head - d->txb_tail;
}

static bool tx_admit(const drv_t *d)
{
    return d->hw_started && !d->tx_stopped && !d->busoff_hold;
}

static void tx_set_prio(drv_t *d)
{
#ifdef CTUCAN_FI
    if (d->fi_fixedprio) {
        ctucan_wr(&d->hw, CTUCAN_TX_PRIORITY, 0x4321u);
        return;
    }
#endif
    ctucan_hw_tx_prio(&d->hw, d->ntxb, d->txb_tail % d->ntxb);
}

/* Takes the next queued frame, round robin across TX mailboxes. */
static canmsg_t *tx_dequeue(drv_t *d, mbox_t **from)
{
    for (unsigned n = 0; n < d->ntx; n++) {
        unsigned i = (d->tx_rr + n) % d->ntx;
        canmsg_t *e = canmsg_dequeue_element(d->tx[i].cdev.msg_queue);
        if (e != NULL) {
            d->tx_rr = (i + 1) % d->ntx;
            *from = &d->tx[i];
            return e;
        }
    }
    return NULL;
}

static void tx_kick(drv_t *d)
{
    unsigned ready = 0;

    while (tx_admit(d) && tx_inflight(d) < d->ntxb) {
        unsigned b = d->txb_head % d->ntxb;
        if (!ctucan_txt_writable(ctucan_hw_txt_state(&d->hw, b))) {
            break;  /* its state change interrupt brings us back */
        }
        mbox_t *t;
        canmsg_t *e = tx_dequeue(d, &t);
        if (e == NULL) {
            break;
        }
        CAN_MSG m = e->cmsg;
        canmsg_queue_element(t->cdev.free_queue, e);

        if (m.mid == CAN_MSG_MID_UNKNOWN) {
            /* plain write(): the mailbox decides id and format, and the frame
             * carries data; nothing else in the element is meaningful */
            m.mid = t->mid;
            m.ext.is_extended_mid = t->ext;
            m.ext.is_remote_frame = 0;
        }
        bool fdf = d->fd && (d->fd_always || m.len > CAN_MSG_DATA_MAX_CAN);
        int rc = ctucan_hw_txt_write(&d->hw, b, &m, fdf, fdf && d->brs);
        if (rc != EOK) {
            d->x.tx_failed++;   /* refused by the hardware layer (format) */
            can_slogf(SLOG2_ERROR, DRV_NAME ": tx%u: dropped mid 0x%08x len %u%s: %s", t->cdev.dev_unit,
                      m.mid, m.len, m.ext.is_remote_frame ? " RTR" : "", strerror(rc));
            continue;
        }
        txslot_t *s = &d->txs[b];
        s->seq = d->tx_seq++;
        s->mid = m.mid;
        s->len = m.len;
        s->ext = m.ext.is_extended_mid != 0;
        s->rtr = m.ext.is_remote_frame != 0;
        s->late = false;
        d->txb_head++;
        d->x.tx_started++;
        ready |= 1u << b;
        if (tx_inflight(d) > d->x.tx_max_inflight) {
            d->x.tx_max_inflight = tx_inflight(d);
        }
    }
    if (ready != 0) {
        /* one command for all of them: the core then picks by priority */
        ctucan_hw_txt_cmd(&d->hw, TXC_TXCR, ready);
        if (ready & (ready - 1)) {
            d->x.tx_batches++;
        }
    }
}

/* Retires finished TXT buffers in FIFO order, judged from one TX_STATUS
 * snapshot. At most ntxb (4) buffers, so this needs no budget, and after
 * TXBHCI has been acknowledged no finished buffer is ever left behind. */
static unsigned tx_retire(drv_t *d)
{
    uint32_t ts = ctucan_rd(&d->hw, CTUCAN_TX_STATUS);
    unsigned n = 0;

    while (tx_inflight(d) > 0) {
        unsigned b = d->txb_tail % d->ntxb;
        unsigned st = TXT_STATE(ts, b);

        switch (st) {
        case TXT_TOK:
            d->stats.transmitted_frames++;
            d->x.tx_ok++;
            break;
        case TXT_ERR:   /* retransmission limit, or bus-off on cores that fail buffers then */
        case TXT_PER:
            d->x.tx_failed++;
            d->stats.total_frame_errors++;
            break;
        case TXT_ABT:
            d->x.tx_aborted++;
            break;
        case TXT_ETY:
            /* the buffer lost its frame without reaching a final state */
            if (d->x.tx_lost++ == 0) {
                can_slogf(SLOG2_ERROR, DRV_NAME ": TXT buffer %u empty while frame %u was in flight", b,
                          d->txs[b].seq);
            }
            break;
        default:
            /* Ready, in transmission or aborting. Later buffers have lower
             * priority, so one of them finishing first is an ordering fault. */
            for (unsigned k = d->txb_tail + 1; k != d->txb_head; k++) {
                unsigned o = k % d->ntxb;
                if (!d->txs[o].late && TXT_STATE(ts, o) == TXT_TOK) {
                    d->txs[o].late = true;
                    d->x.tx_order_err++;
                }
            }
            return n;
        }
        d->txb_tail++;
        if (d->ntxb > 1) {
            tx_set_prio(d);
        }
        if (st != TXT_ETY) {
            ctucan_hw_txt_cmd(&d->hw, TXC_TXCE, 1u << b);
        }
        n++;
    }
    return n;
}

static void tx_abort_inflight(drv_t *d)
{
    uint32_t ts = ctucan_rd(&d->hw, CTUCAN_TX_STATUS);
    unsigned mask = 0;

    for (unsigned k = d->txb_tail; k != d->txb_head; k++) {
        unsigned b = k % d->ntxb;
        unsigned st = TXT_STATE(ts, b);
        if (st == TXT_RDY || st == TXT_TRAN) {
            mask |= 1u << b;
        }
    }
    if (mask != 0) {
        ctucan_hw_txt_cmd(&d->hw, TXC_TXCA, mask);
    }
}

/* Gives up on TXT buffers that are still not final: they are counted as
 * aborted and the ring moves on. tx_kick() checks a buffer is writable before
 * it uses it again, so a buffer that is still aborting is not overwritten. */
static void tx_force_retire(drv_t *d)
{
    unsigned n = tx_inflight(d);

    if (n == 0) {
        return;
    }
    d->x.tx_abort_forced += n;
    d->x.tx_aborted += n;
    can_slogf(SLOG2_ERROR, DRV_NAME ": %u TXT buffer(s) did not finish or abort in time, dropped", n);
    d->txb_tail = d->txb_head;
    if (d->ntxb > 1) {
        tx_set_prio(d);
    }
}

/* Stops TX admission and waits for the TXT buffers to finish: up to `ms` for
 * a normal completion, then an abort and up to `ms` again. Frames that are
 * aborted are dropped and counted; frames still in libcan's queues stay there. */
static int tx_quiesce(drv_t *d, unsigned ms)
{
    d->tx_stopped = true;
    for (int phase = 0; phase < 2; phase++) {
        uint64_t end = now_ms() + ms;
        for (;;) {
            fi_tick(d);
            tx_retire(d);
            if (tx_inflight(d) == 0) {
                return EOK;
            }
            if (now_ms() >= end) {
                break;
            }
            struct timespec ts = { 0, 200000 };
            nanosleep(&ts, NULL);
        }
        if (phase == 0) {
            tx_abort_inflight(d);
        }
    }
    tx_force_retire(d);
    return ETIMEDOUT;
}

static void cb_transmit(CANDEV *cdev)
{
    tx_kick(container_of(cdev, mbox_t, cdev)->drv);
}

/* ---- receive ----------------------------------------------------------- */

static void rx_deliver(drv_t *d, const CAN_MSG *m)
{
    for (unsigned i = 0; i < d->nrx; i++) {
        mbox_t *r = &d->rx[i];

        if (r->mfilter != 0 && ((m->mid ^ r->mid) & r->mfilter) != 0) {
            continue;
        }
        if (r->mfilter != 0 && (m->ext.is_extended_mid != 0) != r->ext) {
            continue;   /* a filtering mailbox takes one identifier format only */
        }
        canmsg_t *e = canmsg_dequeue_element(r->cdev.free_queue);
        if (e == NULL) {
            /* queue full: drop the oldest frame, keep the newest */
            e = canmsg_dequeue_element(r->cdev.msg_queue);
            d->stats.sw_receive_q_full++;
            if (e == NULL) {
                continue;
            }
        }
        e->cmsg = *m;
        canmsg_queue_element(r->cdev.msg_queue, e);
    }
}

static void rx_flush(drv_t *d)
{
    unsigned lost = ctucan_hw_rx_frames(&d->hw);

    ctucan_hw_rx_flush(&d->hw);
    d->x.rx_flushes++;
    d->x.rx_flushed += lost;
    d->stats.total_frame_errors++;
    can_slogf(SLOG2_ERROR, DRV_NAME ": RX format word does not match its frame, FIFO flushed (%u frame(s))", lost);
}

static unsigned rx_drain(drv_t *d, unsigned budget)
{
    unsigned n = 0;

    while (n < budget && ctucan_hw_rx_frames(&d->hw) > 0) {
        CAN_MSG m;
        ctucan_flags_t fl;

        if (ctucan_hw_rx(&d->hw, &m, &fl) != EOK) {
            /* nothing past the format word was consumed; where the next frame
             * starts is unknown, so everything in the FIFO goes */
            rx_flush(d);
            break;
        }
        n++;
        m.ext.timestamp = now_us(d);
        d->stats.received_frames++;
        rx_deliver(d, &m);
    }
    return n;
}

/* ---- errors ------------------------------------------------------------ */

/* Bus-off: TX is held and the frames in TXT buffers are aborted. The aborts
 * are waited for here, at most TX_QUIESCE_MS, and whatever is still not final
 * then is given up; nothing relies on later traffic or a timer for that. */
static void enter_busoff(drv_t *d, uint32_t cnt)
{
    d->stats.bus_off_state_count++;
    d->busoff_hold = true;
    can_slogf(SLOG2_WARNING, DRV_NAME ": bus-off (TEC %u REC %u): TX held, %u frame(s) in TXT buffers aborted%s",
              CNT_TEC(cnt), CNT_REC(cnt), tx_inflight(d), d->autobus ? ", recovery requested" : "");
    tx_abort_inflight(d);
    uint64_t end = now_ms() + TX_QUIESCE_MS;
    for (;;) {
        fi_tick(d);
        tx_retire(d);
        if (tx_inflight(d) == 0 || now_ms() >= end) {
            break;
        }
        struct timespec ts = { 0, 200000 };
        nanosleep(&ts, NULL);
    }
    tx_force_retire(d);
    if (d->autobus) {
        ctucan_wr(&d->hw, CTUCAN_COMMAND, CMD_ERCRST);
    }
}

static void error_event(drv_t *d, uint32_t st)
{
    if (st & INT_BEI) {
        d->stats.total_frame_errors++;
        switch (ERR_CAPT_TYPE(ctucan_rd(&d->hw, CTUCAN_ERR_CAPT))) {
        case ERRT_BIT:   d->stats.dom_bit_recess_errors++; break;
        case ERRT_CRC:   d->stats.crc_errors++; break;
        case ERRT_FORM:  d->stats.form_errors++; break;
        case ERRT_ACK:   d->stats.missing_ack++; break;
        case ERRT_STUFF: d->stats.stuff_errors++; break;
        default: break;
        }
    }
    if (st & INT_EWLI) {
        /* EWLI fires on the way up and on the way down; STATUS.EWL tells which */
        bool ewl = (ctucan_rd(&d->hw, CTUCAN_STATUS) & STAT_EWL) != 0;
        if (ewl != d->ewl) {
            uint32_t cnt = ctucan_rd(&d->hw, CTUCAN_REC_TEC);
            if (ewl) {
                d->stats.error_warning_state_count++;
                d->x.ewl_up++;
            } else {
                d->x.ewl_down++;
            }
            can_slogf(SLOG2_WARNING, DRV_NAME ": error warning limit %s (TEC %u REC %u)",
                      ewl ? "reached" : "cleared", CNT_TEC(cnt), CNT_REC(cnt));
            d->ewl = ewl;
        }
    }
    uint32_t fs = ctucan_rd(&d->hw, CTUCAN_EWL_ERP_FAULT) & FS_MASK;
    if (fs != d->fs) {
        uint32_t cnt = ctucan_rd(&d->hw, CTUCAN_REC_TEC);

        d->fs = fs;
        if (fs & FS_BOF) {
            if (!d->busoff_hold) {
                enter_busoff(d, cnt);
            }
        } else if (fs & FS_ERP) {
            d->stats.error_passive_state_count++;
            can_slogf(SLOG2_WARNING, DRV_NAME ": error passive (TEC %u REC %u)%s", CNT_TEC(cnt), CNT_REC(cnt),
                      d->busoff_hold ? ", TX still held after bus-off" : "");
        } else if (fs & FS_ERA) {
            if (d->busoff_hold) {
                d->busoff_hold = false;
                d->x.busoff_recovered++;
                can_slogf(SLOG2_WARNING, DRV_NAME ": bus-off recovery confirmed, TX resumes");
            }
        }
        /* any other value (none of the three bits) leaves a bus-off hold in place */
        tx_kick(d);
    }
}

/* ---- interrupt service ------------------------------------------------- */

static void suppress(drv_t *d)
{
    if (!d->suppressed) {
        ctucan_wr(&d->hw, CTUCAN_INT_ENA_CLR, d->int_ena);
        d->suppressed = true;
    }
}

static void unsuppress(drv_t *d)
{
    if (d->suppressed) {
        ctucan_wr(&d->hw, CTUCAN_INT_ENA_SET, d->int_ena);
        d->suppressed = false;
    }
}

/* One bounded batch. TX completions and error events are handled in full
 * (four TXT buffers at most); RX moves at most d->budget frames. RBNEI is not
 * acknowledged here: finish_pulse() does that once the readers are woken. */
static void service(drv_t *d)
{
    unsigned budget = d->budget, moved = 0;

    for (unsigned pass = 0; pass < PASS_LIMIT; pass++) {
        uint32_t st = ctucan_rd(&d->hw, CTUCAN_INT_STAT) & d->int_ena;
        bool rx = budget > 0 && ctucan_hw_rx_frames(&d->hw) > 0;

        if ((st & ~INT_RBNEI) == 0 && !rx) {
            if (pass == 0 && st == 0) {
                d->x.spurious++;    /* another device on a shared line, or already handled */
            }
            break;
        }
        d->stats.total_interrupts++;
        if (st & INT_DOI) {
            /* datasheet: clear STATUS.DOR first, or DOI is set again */
            ctucan_wr(&d->hw, CTUCAN_COMMAND, CMD_CDO);
            ctucan_wr(&d->hw, CTUCAN_INT_STAT, INT_DOI);
            d->stats.hw_receive_overflows++;
        }
        if (st & (INT_EWLI | INT_FCSI | INT_BEI)) {
            ctucan_wr(&d->hw, CTUCAN_INT_STAT, st & (INT_EWLI | INT_FCSI | INT_BEI));
            error_event(d, st);
        }
        if (st & INT_TXBHCI) {
            /* ack before the snapshot: a buffer that changes later sets it again */
            ctucan_wr(&d->hw, CTUCAN_INT_STAT, INT_TXBHCI);
            d->stats.tx_interrupts++;
            tx_retire(d);
            tx_kick(d);     /* also when nothing retired: a busy buffer may have become writable */
        }
        if (rx) {
            unsigned n = rx_drain(d, budget);
            d->stats.rx_interrupts++;
            budget -= n;
            moved += n;
        }
    }
    if (moved > d->x.rx_max_batch) {
        d->x.rx_max_batch = moved;
    }
}

/* A mailbox whose returns keep waking nobody: its queued frames are dropped
 * and counted, so no blocked reader is left behind frames that cannot reach
 * it and RBNEI is not held for ever. The readers wait for the next frame,
 * whose pulse returns the mailbox again. */
static void rx_unstick(drv_t *d, unsigned i)
{
    mbox_t *r = &d->rx[i];
    unsigned n = 0;
    canmsg_t *e;

    while ((e = canmsg_dequeue_element(r->cdev.msg_queue)) != NULL) {
        canmsg_queue_element(r->cdev.free_queue, e);
        n++;
    }
    d->x.notify_stuck++;
    d->x.rx_discarded += n;
    r->stuck = 0;
    can_slogf(SLOG2_ERROR, DRV_NAME ": rx%u: %u return(s) in a row woke no reader (%zu blocked); %u queued frame(s) dropped",
              r->cdev.dev_unit, STUCK_LIMIT, r->cdev.wait_client_queue->cnt, n);
}

/* libcan wakes a blocked reader per returned RX device (the DDK says the one
 * blocked longest), so a mailbox is returned as long as it has both queued
 * frames and blocked readers. */
static CANDEV *next_report(drv_t *d)
{
    if (d->last_rep != NULL) {
        CANDEV *c = d->last_rep;
        unsigned i = (unsigned)(container_of(c, mbox_t, cdev) - d->rx);
        if (c->msg_queue->msgcnt >= d->last_msgcnt && c->wait_client_queue->cnt >= d->last_waitcnt) {
            /* nobody woke: not again in this pulse, but in the next ones,
             * up to STUCK_LIMIT pulses in a row */
            d->rep_stuck |= 1u << i;
            if (++d->rx[i].stuck < STUCK_LIMIT) {
                d->notify_left = true;
            } else {
                rx_unstick(d, i);
            }
        } else {
            d->rx[i].stuck = 0;
        }
        d->last_rep = NULL;
    }
    for (unsigned n = 0; n < d->nrx; n++) {
        unsigned i = (d->report_rr + n) % d->nrx;
        CANDEV *c = &d->rx[i].cdev;

        if ((d->rep_stuck & (1u << i)) || c->msg_queue->msgcnt == 0 || c->wait_client_queue->cnt == 0) {
            continue;
        }
        if (d->reports_left == 0) {
            d->notify_left = true;  /* the next interrupt pulse goes on with them */
            return NULL;
        }
        d->reports_left--;
        d->x.reports++;
        d->report_rr = (i + 1) % d->nrx;
        d->last_rep = c;
        d->last_msgcnt = c->msg_queue->msgcnt;
        d->last_waitcnt = c->wait_client_queue->cnt;
        return c;
    }
    return NULL;
}

static void finish_pulse(drv_t *d)
{
    /* RBNEI stays set while reader notifications are left, so the core
     * interrupts again and the next pulse continues them. Once they are done
     * it is acknowledged; the core sets it again at once if frames are still
     * in the FIFO (datasheet: RBNEI re-activates until the buffer is read). */
    if (d->notify_left) {
        d->x.notify_deferred++;
    } else {
        ctucan_wr(&d->hw, CTUCAN_INT_STAT, INT_RBNEI);
    }

    /* The core's interrupt output goes off before the line is unmasked and on
     * again afterwards. Whatever is still pending then raises the line afresh
     * and brings the next pulse; nothing depends on how the interrupt
     * controller treats a line that is already asserted when it is unmasked
     * (section 3.3). */
    suppress(d);
    if (InterruptUnmask(0, d->iid) == -1 && d->x.unmask_err++ == 0) {
        can_slogf(SLOG2_ERROR, DRV_NAME ": InterruptUnmask: %s", strerror(errno));
    }
    if (!d->hw_started) {
        /* A stopped core (failed reset, shutdown) is not serviced, so its
         * output stays off: pending status must not bring pulse after pulse.
         * The line itself is unmasked above, as this pulse requires. */
        d->x.stopped_pulses++;
        return;
    }
    if ((ctucan_rd(&d->hw, CTUCAN_INT_STAT) & d->int_ena) != 0) {
        d->x.rearm_pending++;
        if (d->notify_left || ctucan_hw_rx_frames(&d->hw) > 0) {
            d->x.deferred++;
        }
    }
    unsuppress(d);
}

static CANDEV *cb_event(void *const hdl, const int code)
{
    drv_t *d = hdl;

    if (!d->in_event) {
        if (code != PULSE_CODE_IRQ) {
            can_slogf(SLOG2_WARNING, DRV_NAME ": unexpected pulse code %d", code);
            return NULL;
        }
        d->x.hw_pulses++;
        d->in_event = true;
        d->notify_left = false;
        fi_tick(d);
        if (d->hw_started) {
            service(d);
        }
        d->reports_left = d->report_budget;
        d->rep_stuck = 0;
        d->last_rep = NULL;
    }
    CANDEV *c;
    while ((c = next_report(d)) != NULL) {
#ifdef CTUCAN_FI
        if (d->fi_nowake) {
            continue;   /* as if libcan had woken nobody */
        }
#endif
        return c;
    }
    finish_pulse(d);
    d->in_event = false;
    return NULL;
}

/* ---- fault injection (FI builds) --------------------------------------- */

#ifdef CTUCAN_FI
static void fi_tick(drv_t *d)
{
    if (d->hw.fi != NULL) {
        ctucan_fi_tick(d->hw.fi, now_ms());
    }
}

/* CTUCAN_DEVCTL_FI_TICK, sent by a test client every millisecond while a
 * scenario runs: advances the script and handles the injected error and TX
 * events. RX is left to interrupt pulses, which also wake the readers. */
static void fi_devctl_tick(drv_t *d)
{
    fi_tick(d);
    if (!d->hw_started) {
        return;
    }
    uint32_t st = ctucan_rd(&d->hw, CTUCAN_INT_STAT) & d->int_ena & (INT_TXBHCI | INT_EWLI | INT_FCSI | INT_BEI);
    if (st & (INT_EWLI | INT_FCSI | INT_BEI)) {
        ctucan_wr(&d->hw, CTUCAN_INT_STAT, st & (INT_EWLI | INT_FCSI | INT_BEI));
        error_event(d, st);
    }
    if (st & INT_TXBHCI) {
        ctucan_wr(&d->hw, CTUCAN_INT_STAT, INT_TXBHCI);
        tx_retire(d);
        tx_kick(d);
    }
}

static int fi_request(drv_t *d, const ctucan_fi_req_t *r)
{
    int rc = ctucan_fi_start(d->hw.fi, r->scenario, r->arg, r->arg2, now_ms());

    if (rc != EOK) {
        return rc;
    }
    d->budget = r->budget != 0 ? r->budget : RX_BUDGET;
    d->report_budget = (r->flags & CTUCAN_FI_F_REPORT1) ? 1 : REPORT_BUDGET;
    d->fi_fixedprio = (r->flags & CTUCAN_FI_F_FIXEDPRIO) != 0;
    d->fi_nowake = (r->flags & CTUCAN_FI_F_NOWAKE) != 0;
    tx_set_prio(d);
    d->fs = ctucan_rd(&d->hw, CTUCAN_EWL_ERP_FAULT) & FS_MASK;
    d->ewl = (ctucan_rd(&d->hw, CTUCAN_STATUS) & STAT_EWL) != 0;
    can_slogf(SLOG2_INFO, DRV_NAME ": fault injection: scenario %u arg %u/%u budget %u flags 0x%x", r->scenario,
              r->arg, r->arg2, d->budget, r->flags);
    fi_devctl_tick(d);  /* a scenario that starts at once takes effect without a ticker */
    return EOK;
}
#else
static void fi_tick(drv_t *d)
{
    (void)d;
}
#endif

/* ---- devctl ------------------------------------------------------------ */

static int restart_controller(drv_t *d)
{
    int rc = d->hw_started ? tx_quiesce(d, TX_QUIESCE_MS) : EOK;

    /* the reset empties the RX FIFO: count what it held */
    d->x.rx_flushed += ctucan_hw_rx_frames(&d->hw);
    ctucan_hw_stop(&d->hw);
    d->hw_started = false;
    d->suppressed = true;       /* ctucan_hw_stop() cleared INT_ENA */
    int rrc = ctucan_hw_reset(&d->hw);
    if (rrc != EOK) {
        d->x.restart_fail++;
        can_slogf(SLOG2_ERROR, DRV_NAME ": controller reset timed out, core left stopped");
        return rrc;
    }
    ctucan_hw_start(&d->hw, &d->cfg, d->int_ena);
    d->suppressed = false;
    d->txb_head = d->txb_tail = 0;
    tx_set_prio(d);
    d->fs = ctucan_rd(&d->hw, CTUCAN_EWL_ERP_FAULT) & FS_MASK;
    d->ewl = (ctucan_rd(&d->hw, CTUCAN_STATUS) & STAT_EWL) != 0;
    d->busoff_hold = false;     /* the reset clears the error counters */
    d->tx_stopped = false;
    d->hw_started = true;       /* only now: the core is out of reset and set up again */
    d->x.restarts++;
    tx_kick(d);
    return rc == ETIMEDOUT ? EOK : rc;  /* the restart happened; the timeout is counted */
}

static int set_timing(drv_t *d, const CAN_DEVCTL_TIMING *t)
{
    unsigned ts1 = t->time_segment_1, ts2 = t->time_segment_2;
    uint32_t clk = t->ref_clock_freq != 0 ? t->ref_clock_freq : d->clk_hz;
    ctucan_bt_t bt, data;

    if (t->bit_rate_prescaler == 0 || ts1 < 2 || ts2 < 1) {
        return EINVAL;
    }
    bt.brp = t->bit_rate_prescaler;
    bt.ph1 = (uint8_t)(ts1 / 2 > 63 ? 63 : ts1 / 2);
    bt.prop = (uint8_t)(ts1 - bt.ph1 > 255 ? 255 : ts1 - bt.ph1);
    bt.ph2 = (uint8_t)ts2;
    bt.sjw = t->sync_jump_width;
    if (ctucan_bt_check(&bt, false) != EOK) {
        return EINVAL;
    }
    uint32_t rate = clk / ((uint32_t)bt.brp * (1u + ts1 + ts2));
    if (rate < 5000 || rate > 1000000) {
        return EINVAL;  /* nominal CAN bit rates */
    }
    data = d->cfg.data;
    if (clk != d->clk_hz && ctucan_bt_calc(clk, d->dbitrate, d->dsp, true, &data) != EOK) {
        return EINVAL;  /* the data phase must still work at the new clock */
    }
    d->clk_hz = clk;
    d->cfg.nominal = bt;
    d->cfg.data = data;
    d->bitrate = rate;
    int rc = restart_controller(d);
    can_slogf(SLOG2_INFO, DRV_NAME ": timing brp %u ts1 %u ts2 %u sjw %u -> %u bit/s%s", bt.brp, ts1, ts2,
              bt.sjw, d->bitrate, rc == EOK ? "" : " (restart failed)");
    return rc;
}

static void dump_regs(const drv_t *d)
{
    static const struct { const char *name; uint32_t off; } regs[] = {
        { "DEVICE_ID", CTUCAN_DEVICE_ID }, { "MODE", CTUCAN_MODE }, { "STATUS", CTUCAN_STATUS },
        { "INT_STAT", CTUCAN_INT_STAT }, { "INT_ENA", CTUCAN_INT_ENA_SET }, { "INT_MASK", CTUCAN_INT_MASK_SET },
        { "BTR", CTUCAN_BTR }, { "BTR_FD", CTUCAN_BTR_FD }, { "FAULT", CTUCAN_EWL_ERP_FAULT },
        { "REC_TEC", CTUCAN_REC_TEC }, { "RX_MEM_INFO", CTUCAN_RX_MEM_INFO }, { "RX_STATUS", CTUCAN_RX_STATUS },
        { "TX_STATUS", CTUCAN_TX_STATUS }, { "TX_PRIORITY", CTUCAN_TX_PRIORITY },
        { "RX_FR_CTR", CTUCAN_RX_FR_CTR }, { "TX_FR_CTR", CTUCAN_TX_FR_CTR },
    };
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        can_slogf(SLOG2_INFO, DRV_NAME ": %-12s 0x%08x", regs[i].name, ctucan_rd(&d->hw, regs[i].off));
    }
    can_slogf(SLOG2_INFO, DRV_NAME ": pulses %u, deferred %u, unmask_err %u, in flight %u, bus-off hold %d",
              d->x.hw_pulses, d->x.deferred, d->x.unmask_err, tx_inflight(d), d->busoff_hold);
}

static int cb_devctl(CANDEV *cdev, io_devctl_t *msg)
{
    mbox_t *mb = container_of(cdev, mbox_t, cdev);
    drv_t *d = mb->drv;
    const CAN_DCMD_DATA *in = _IO_INPUT_PAYLOAD(msg);
    CAN_DCMD_DATA *out = _IO_OUTPUT_PAYLOAD(msg);
    size_t nbytes = msg->i.nbytes;  /* what the client sent with a DIOT command */
    bool is_rx = cdev->devtype == CANDEV_TYPE_RX;
    int rc;

#define NEED(n) if (nbytes < (n)) { rc = -EINVAL; break; }
    switch (msg->i.dcmd) {
    case CAN_DEVCTL_GET_MID:
        out->mid = mb->mid;
        rc = sizeof(out->mid);
        break;
    case CAN_DEVCTL_SET_MID:
        NEED(sizeof(in->mid));
        mb->mid = in->mid & (mb->ext ? IDW_EXT_MASK : IDW_STD_MASK);
        rc = EOK;
        break;
    case CAN_DEVCTL_GET_MFILTER:
        out->mfilter = mb->mfilter;
        rc = is_rx ? (int)sizeof(out->mfilter) : -EINVAL;
        break;
    case CAN_DEVCTL_SET_MFILTER:
        NEED(sizeof(in->mfilter));
        if (is_rx) {
            mb->mfilter = in->mfilter & IDW_EXT_MASK;
        }
        rc = is_rx ? EOK : -EINVAL;
        break;
    case CAN_DEVCTL_GET_TIMESTAMP:
        out->timestamp = now_us(d);
        rc = sizeof(out->timestamp);
        break;
    case CAN_DEVCTL_SET_TIMESTAMP:
        NEED(sizeof(in->timestamp));
        d->ts_delta += (int64_t)in->timestamp - (int64_t)now_us(d);
        rc = EOK;
        break;
    case CAN_DEVCTL_GET_INFO: {
        CAN_DEVCTL_INFO *info = &out->info;
        memset(info, 0, sizeof(*info));
        snprintf(info->description, sizeof(info->description), "CTU CAN FD %u.%u PCI %04x:%04x core %u%s",
                 d->ver_major, d->ver_minor, CTUCAN_PCI_VENDOR, CTUCAN_PCI_DEVICE, d->core, d->fd ? " FD" : "");
        info->msgq_size = d->msgq;
        info->waitq_size = d->waitq;
        info->mode = CANDEV_MODE_IO;
        info->bit_rate = d->bitrate;
        info->num_tx_mboxes = d->ntx;
        info->num_rx_mboxes = d->nrx;
        info->loopback_internal = d->cfg.loopback;
        info->autobus_on = d->autobus;
        info->silent = d->cfg.listen_only;
        info->bit_rate_prescaler = d->cfg.nominal.brp;
        info->sync_jump_width = d->cfg.nominal.sjw;
        info->time_segment_1 = (uint8_t)(d->cfg.nominal.prop + d->cfg.nominal.ph1);
        info->time_segment_2 = d->cfg.nominal.ph2;
        rc = sizeof(*info);
        break;
    }
    case CAN_DEVCTL_GET_STATS:
        out->stats = d->stats;
        rc = sizeof(out->stats);
        break;
    case CAN_DEVCTL_ERROR:
        out->error.drvr1 = ctucan_rd(&d->hw, CTUCAN_EWL_ERP_FAULT);
        out->error.drvr2 = ctucan_rd(&d->hw, CTUCAN_REC_TEC);
        out->error.drvr3 = ctucan_rd(&d->hw, CTUCAN_ERR_NORM_FD);
        out->error.drvr4 = ctucan_rd(&d->hw, CTUCAN_STATUS);
        rc = sizeof(out->error);
        break;
    case CAN_DEVCTL_SET_TIMING:
        NEED(sizeof(in->timing));
        rc = -set_timing(d, &in->timing);
        break;
    case CAN_DEVCTL_DEBUG_INFO:
    case CAN_DEVCTL_DEBUG_INFO2:
        dump_regs(d);
        rc = EOK;
        break;
    case CAN_DEVCTL_WRITE_CANMSG_EXT:
    case CAN_DEVCTL_TX_FRAME_RAW: {
        NEED(sizeof(in->canmsg));
        const CAN_MSG *m = &in->canmsg;
        if (is_rx || m->len > (d->fd ? CAN_MSG_DATA_MAX_CANFD : CAN_MSG_DATA_MAX_CAN)) {
            rc = -EINVAL;
        } else if (m->ext.is_remote_frame && (m->len > CAN_MSG_DATA_MAX_CAN || d->fd_always)) {
            rc = -EINVAL;   /* such a frame would be CAN FD, which has no remote frames */
        } else {
            rc = -ENOTSUP;  /* libcan queues it and calls transmit() */
        }
        break;
    }
    case CAN_DEVCTL_READ_CANMSG_EXT:
    case CAN_DEVCTL_RX_FRAME_RAW_BLOCK:
    case CAN_DEVCTL_RX_FRAME_RAW_NOBLOCK:
        rc = is_rx ? -ENOTSUP : -EINVAL;
        break;
    case CTUCAN_DEVCTL_GET_XSTATS:
        d->x.state = d->fs | (d->busoff_hold ? 1u << 9 : 0) | (d->ewl ? 1u << 8 : 0) | (tx_inflight(d) & 0xffu);
#ifdef CTUCAN_FI
        d->x.state |= (ctucan_fi_state(d->hw.fi) & 0xffu) << 24;
#endif
        memcpy(out, &d->x, sizeof(d->x));
        rc = sizeof(d->x);
        break;
    case CTUCAN_DEVCTL_RECOVER:
        /* manual bus-off recovery when -a is not set; TX resumes once FAULT_STATE reads error active */
        if (d->fs & FS_BOF) {
            ctucan_wr(&d->hw, CTUCAN_COMMAND, CMD_ERCRST);
            can_slogf(SLOG2_WARNING, DRV_NAME ": bus-off recovery requested");
        }
        rc = EOK;
        break;
#ifdef CTUCAN_FI
    case CTUCAN_DEVCTL_FI:
        NEED(sizeof(ctucan_fi_req_t));
        rc = -fi_request(d, (const ctucan_fi_req_t *)in);
        break;
    case CTUCAN_DEVCTL_FI_TICK:
        fi_devctl_tick(d);
        rc = EOK;
        break;
#endif
    default:
        rc = -ENOTSUP;
        break;
    }
#undef NEED
    if (rc < 0) {
        errno = -rc;
        rc = -1;
    }
    return rc;
}

static can_drvr_funcs_t g_funcs = {
    .transmit = cb_transmit,
    .devctl = cb_devctl,
    .event_handler = cb_event,
};

static void usage(void)
{
    printf("usage: " DRV_NAME " [options]\n"
           "  -i idx    PCI device index among %04x:%04x boards (0)\n"
           "  -c core   CTU CAN FD core on the board (0)\n"
           "  -u unit   creates /dev/can<unit>/rxN and txN (core + 1)\n"
           "  -r n      RX mailboxes (1)       -t n  TX mailboxes (1)\n"
           "  -q n      queue length per mailbox (64)  -w n  blocked clients per mailbox (16)\n"
           "  -T n      TXT buffers in use, 1 to 4 (4)\n"
           "  -x        mailbox MIDs are 29-bit identifiers (default 11-bit)\n"
           "  -b rate   nominal bit rate (500000)  -d rate  data bit rate (2000000)\n"
           "  -k hz     core clock (100000000)\n"
           "  -C        classic CAN only (default: CAN FD, 64-byte payloads)\n"
           "  -A        send every frame as CAN FD (default: only frames over 8 bytes)\n"
           "  -S        no bit rate switch in CAN FD frames\n"
           "  -l        internal loopback   -L  listen only   -a  automatic bus-off recovery\n"
           "  -v        more verbose slog2 output (repeat)\n",
           CTUCAN_PCI_VENDOR, CTUCAN_PCI_DEVICE);
}

static int parse_args(drv_t *d, int argc, char *argv[])
{
    int c;

    d->nrx = d->ntx = 1;
    d->msgq = 64;
    d->waitq = 16;
    d->ntxb = 4;
    d->verbosity = SLOG2_INFO;
    d->fd = d->brs = true;
    d->clk_hz = 100000000u;
    d->bitrate = 500000u;
    d->dbitrate = 2000000u;
    d->sp = 875;
    d->dsp = 750;
    d->unit = 0;
    while ((c = getopt(argc, argv, "i:c:u:r:t:q:w:T:b:d:k:xCASlLavh")) != -1) {
        switch (c) {
        case 'i': d->pci_index = strtoul(optarg, NULL, 0); break;
        case 'c': d->core = strtoul(optarg, NULL, 0); break;
        case 'u': d->unit = strtoul(optarg, NULL, 0); break;
        case 'r': d->nrx = strtoul(optarg, NULL, 0); break;
        case 't': d->ntx = strtoul(optarg, NULL, 0); break;
        case 'q': d->msgq = strtoul(optarg, NULL, 0); break;
        case 'w': d->waitq = strtoul(optarg, NULL, 0); break;
        case 'T': d->ntxb = strtoul(optarg, NULL, 0); break;
        case 'x': d->ext = true; break;
        case 'b': d->bitrate = strtoul(optarg, NULL, 0); break;
        case 'd': d->dbitrate = strtoul(optarg, NULL, 0); break;
        case 'k': d->clk_hz = strtoul(optarg, NULL, 0); break;
        case 'C': d->fd = false; break;
        case 'A': d->fd_always = true; break;
        case 'S': d->brs = false; break;
        case 'l': d->cfg.loopback = true; break;
        case 'L': d->cfg.listen_only = true; break;
        case 'a': d->autobus = true; break;
        case 'v': d->verbosity++; break;
        default: usage(); return EINVAL;
        }
    }
    if (d->nrx < 1 || d->nrx > MAX_RX_DEVS || d->ntx < 1 || d->ntx > MAX_TX_DEVS || d->msgq < 1 ||
        d->waitq < 1 || d->ntxb < 1 || d->ntxb > CTUCAN_TXT_BUFFERS) {
        fprintf(stderr, DRV_NAME ": bad mailbox, queue or TXT buffer count\n");
        return EINVAL;
    }
    if (d->unit == 0) {
        d->unit = d->core + 1;
    }
    d->cfg.fd = d->fd;
    if (ctucan_bt_calc(d->clk_hz, d->bitrate, d->sp, false, &d->cfg.nominal) != EOK ||
        ctucan_bt_calc(d->clk_hz, d->dbitrate, d->dsp, true, &d->cfg.data) != EOK) {
        fprintf(stderr, DRV_NAME ": no bit timing for %u/%u bit/s at %u Hz\n", d->bitrate, d->dbitrate,
                d->clk_hz);
        return EINVAL;
    }
    return EOK;
}

/* BAR0 starts with the bridge id; its low nibble is the number of cores. */
static int pci_count_cores(drv_t *d, const pci_ba_t *bar0)
{
    void *p = mmap_device_memory(NULL, 0x1000, PROT_READ | PROT_NOCACHE, 0, bar0->addr);

    if (p == MAP_FAILED) {
        fprintf(stderr, DRV_NAME ": mmap BAR0: %s\n", strerror(errno));
        return errno;
    }
    uint32_t id = *(volatile uint32_t *)((volatile uint8_t *)p + CTUCAN_BAR0_ID);
    munmap_device_memory(p, 0x1000);
    d->ncores = CTUCAN_BAR0_CORES(id);
    if (d->core >= d->ncores) {
        fprintf(stderr, DRV_NAME ": core %u requested, bridge id 0x%08x reports %u core(s)\n", d->core, id,
                d->ncores);
        return ENODEV;
    }
    return EOK;
}

static int pci_open(drv_t *d)
{
    pci_err_t err;
    pci_bdf_t bdf = pci_device_find(d->pci_index, CTUCAN_PCI_VENDOR, CTUCAN_PCI_DEVICE, PCI_CCODE_ANY);

    if (bdf == PCI_BDF_NONE) {
        fprintf(stderr, DRV_NAME ": no PCI %04x:%04x board at index %u\n", CTUCAN_PCI_VENDOR,
                CTUCAN_PCI_DEVICE, d->pci_index);
        return ENODEV;
    }
    d->pci = pci_device_attach(bdf, pci_attachFlags_EXCLUSIVE_OWNER, &err);
    if (d->pci == NULL) {
        fprintf(stderr, DRV_NAME ": pci_device_attach: %s\n", pci_strerror(err));
        return EBUSY;
    }

    pci_ba_t ba[7];
    int_t nba = 7;
    err = pci_device_read_ba(d->pci, &nba, ba, pci_reqType_e_UNSPECIFIED);
    if (err != PCI_ERR_OK) {
        fprintf(stderr, DRV_NAME ": pci_device_read_ba: %s\n", pci_strerror(err));
        return EIO;
    }

    pci_irq_t irq[4];
    int_t nirq = 4;
    err = pci_device_read_irq(d->pci, &nirq, irq);
    if (err != PCI_ERR_OK || nirq < 1) {
        fprintf(stderr, DRV_NAME ": no interrupt assigned (%s)\n", pci_strerror(err));
        return EIO;
    }
    d->irq = irq[0];

    const pci_ba_t *bar0 = NULL, *bar1 = NULL;
    for (int i = 0; i < nba; i++) {
        if (ba[i].type == pci_asType_e_MEM && ba[i].bar_num == 0) {
            bar0 = &ba[i];
        } else if (ba[i].type == pci_asType_e_MEM && ba[i].bar_num == 1) {
            bar1 = &ba[i];
        }
    }
    if (bar0 == NULL || bar1 == NULL) {
        fprintf(stderr, DRV_NAME ": BAR0 or BAR1 missing\n");
        return ENODEV;
    }
    int rc = pci_count_cores(d, bar0);
    if (rc != EOK) {
        return rc;
    }
    uint64_t off = (uint64_t)d->core * CTUCAN_CORE_STRIDE;
    if (off + CTUCAN_CORE_SPAN > bar1->size) {
        fprintf(stderr, DRV_NAME ": core %u outside BAR1 (size 0x%llx)\n", d->core, (unsigned long long)bar1->size);
        return ENODEV;
    }
    d->map = mmap_device_memory(NULL, CTUCAN_CORE_SPAN, PROT_READ | PROT_WRITE | PROT_NOCACHE, 0, bar1->addr + off);
    if (d->map == MAP_FAILED) {
        d->map = NULL;
        fprintf(stderr, DRV_NAME ": mmap_device_memory: %s\n", strerror(errno));
        return errno;
    }
    d->hw.base = d->map;
    printf(DRV_NAME ": B%u:D%u:F%u BAR1 0x%llx core %u of %u, irq %d\n", PCI_BUS(bdf), PCI_DEV(bdf), PCI_FUNC(bdf),
           (unsigned long long)(bar1->addr + off), d->core, d->ncores, (int)d->irq);
    return EOK;
}

static int devices_init(drv_t *d)
{
    CANDEV_INIT ci;

    memset(&ci, 0, sizeof(ci));
    ci.can_unit = (int)d->unit;
    ci.msgq_size = d->msgq;
    ci.waitq_size = d->waitq;
    ci.mode = CANDEV_MODE_IO;

    /* QNX convention: rx0..rx(n-1) first, TX mailboxes numbered after them */
    for (unsigned i = 0; i < d->nrx + d->ntx; i++) {
        bool rx = i < d->nrx;
        mbox_t *mb = rx ? &d->rx[i] : &d->tx[i - d->nrx];
        unsigned id = 0x100u + (i - d->nrx);    /* default TX id 0x100 + n */

        ci.devtype = rx ? CANDEV_TYPE_RX : CANDEV_TYPE_TX;
        ci.dev_unit = (int)i;
        if (can_resmgr_init_device(&mb->cdev, &ci) != EOK) {
            fprintf(stderr, DRV_NAME ": can_resmgr_init_device %u failed\n", i);
            return EIO;
        }
        mb->inited = true;
        mb->cdev.cflags |= d->fd ? CANDEV_CFLAG_CANFD : CANDEV_CFLAG_CAN;
        mb->drv = d;
        mb->ext = d->ext;
        mb->mid = rx ? 0 : (d->ext ? id : id << 18);
        mb->mfilter = 0;
    }
    for (unsigned i = 0; i < d->nrx + d->ntx; i++) {
        mbox_t *mb = i < d->nrx ? &d->rx[i] : &d->tx[i - d->nrx];
        if (can_resmgr_create_device(&mb->cdev) == -1) {
            fprintf(stderr, DRV_NAME ": can_resmgr_create_device %u: %s\n", i, strerror(errno));
            return EIO;
        }
    }
    return EOK;
}

static int attach_irq(drv_t *d)
{
    d->iid = can_resmgr_attach_intr(d, d->irq, PULSE_CODE_IRQ);
    if (d->iid == -1) {
        fprintf(stderr, DRV_NAME ": can_resmgr_attach_intr(%d): %s\n", (int)d->irq, strerror(errno));
        return EIO;
    }
    return EOK;
}

/* Undoes whatever main() got done, in reverse order. */
static void teardown(drv_t *d)
{
    if (d->hw_started) {
        tx_quiesce(d, TX_QUIESCE_MS);
    }
    if (d->map != NULL) {
        ctucan_hw_stop(&d->hw);     /* also after a failed restart: leave the core off the bus */
        d->hw_started = false;
        d->suppressed = true;
    }
    if (d->iid != -1) {
        can_resmgr_detach_intr(d->iid, PULSE_CODE_IRQ);
        d->iid = -1;
    }
    for (unsigned i = 0; i < d->nrx; i++) {
        if (d->rx[i].inited) {
            can_resmgr_destroy_device(&d->rx[i].cdev);
        }
    }
    for (unsigned i = 0; i < d->ntx; i++) {
        if (d->tx[i].inited) {
            can_resmgr_destroy_device(&d->tx[i].cdev);
        }
    }
    if (d->resmgr_up) {
        can_resmgr_fini();
    }
    if (d->map != NULL) {
        munmap_device_memory(d->map, CTUCAN_CORE_SPAN);
    }
    if (d->pci != NULL) {
        pci_device_detach(d->pci);
    }
}

int main(int argc, char *argv[])
{
    drv_t *d = &g_drv;
    int rc;

    d->iid = -1;
    if ((rc = parse_args(d, argc, argv)) != EOK) {
        return rc;
    }
    /* PCI configuration access needs I/O privileges (ThreadCtl(), QNX 8.0 library reference) */
    if (ThreadCtl(_NTO_TCTL_IO_LEVEL, (void *)(uintptr_t)_NTO_IO_LEVEL_1) == -1) {
        fprintf(stderr, DRV_NAME ": ThreadCtl(_NTO_TCTL_IO_LEVEL): %s\n", strerror(errno));
        return EPERM;
    }
    if ((rc = pci_open(d)) != EOK) {
        goto fail;
    }
    if (ctucan_hw_identify(&d->hw, &d->ver_major, &d->ver_minor) != EOK) {
        fprintf(stderr, DRV_NAME ": DEVICE_ID 0x%08x is not CTU CAN FD\n", ctucan_rd(&d->hw, CTUCAN_DEVICE_ID));
        rc = ENODEV;
        goto fail;
    }
#ifdef CTUCAN_FI
    if ((d->hw.fi = ctucan_fi_create()) == NULL) {
        rc = ENOMEM;
        goto fail;
    }
#endif
    /* A restarted driver must not inherit TX buffers or IRQs from a dead one. */
    if ((rc = ctucan_hw_reset(&d->hw)) != EOK) {
        fprintf(stderr, DRV_NAME ": core did not come out of reset\n");
        goto fail;
    }
    d->int_ena = INT_RBNEI | INT_DOI | INT_TXBHCI | INT_EWLI | INT_FCSI | INT_BEI;
    d->budget = RX_BUDGET;
    d->report_budget = REPORT_BUDGET;

    if (can_resmgr_init(&g_funcs, d->verbosity) != EOK) {
        fprintf(stderr, DRV_NAME ": can_resmgr_init failed\n");
        rc = EIO;
        goto fail;
    }
    d->resmgr_up = true;
    if ((rc = devices_init(d)) != EOK || (rc = attach_irq(d)) != EOK) {
        goto fail;
    }
    ctucan_hw_start(&d->hw, &d->cfg, d->int_ena);
    tx_set_prio(d);
    d->fs = ctucan_rd(&d->hw, CTUCAN_EWL_ERP_FAULT) & FS_MASK;
    d->ewl = (ctucan_rd(&d->hw, CTUCAN_STATUS) & STAT_EWL) != 0;
    d->hw_started = true;

    printf(DRV_NAME ": CTU CAN FD %u.%u, /dev/can%u/rx0..rx%u tx%u..tx%u, %s, %u bit/s (brp %u tq %u)",
           d->ver_major, d->ver_minor, d->unit, d->nrx - 1, d->nrx, d->nrx + d->ntx - 1,
           d->fd ? "CAN FD" : "classic CAN", d->bitrate, d->cfg.nominal.brp,
           1 + d->cfg.nominal.prop + d->cfg.nominal.ph1 + d->cfg.nominal.ph2);
    if (d->fd) {
        printf(", data %u bit/s (brp %u tq %u)%s", d->dbitrate, d->cfg.data.brp,
               1 + d->cfg.data.prop + d->cfg.data.ph1 + d->cfg.data.ph2, d->brs ? " BRS" : "");
    }
    printf(", %u TXT buffer(s)%s\n", d->ntxb,
#ifdef CTUCAN_FI
           ", fault injection build"
#else
           ""
#endif
    );
    fflush(stdout);
    can_slogf(SLOG2_INFO, DRV_NAME ": started on irq %d, unit %u", (int)d->irq, d->unit);

    can_resmgr_start();     /* returns after SIGTERM */

    can_slogf(SLOG2_INFO, DRV_NAME ": stopping, %u frame(s) in TXT buffers", tx_inflight(d));
    teardown(d);
    can_slogf(SLOG2_INFO, DRV_NAME ": stopped: tx %u rx %u, pulses %u, unmask_err %u", d->stats.transmitted_frames,
              d->stats.received_frames, d->x.hw_pulses, d->x.unmask_err);
    return EOK;

fail:
    teardown(d);
    return rc;
}
