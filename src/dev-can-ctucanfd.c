/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * dev-can-ctucanfd: QNX 8.0 CAN driver on the CAN DDK (libcan) for the CTU
 * CAN FD IP core on PCI, including the ctucan_pci board QEMU emulates.
 *
 * libcan runs one resource manager thread. can_resmgr_attach_intr() ties the
 * PCI interrupt to a pulse, and libcan's pulse handler calls event_handler()
 * on that thread (CAN DDK docs). All callbacks run on that thread, so the driver
 * state needs no locks.
 */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
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

#define DRV_NAME        "dev-can-ctucanfd"
#define MAX_RX_DEVS     16
#define MAX_TX_DEVS     16
#define PULSE_CODE_IRQ  _PULSE_CODE_MINAVAIL
#define TXBUF           0u      /* one TXT buffer in flight keeps queue order */
#define RX_BUDGET       512u    /* frames drained per interrupt pass */

#define container_of(p, type, member) ((type *)(void *)((char *)(p) - offsetof(type, member)))

typedef struct drv drv_t;

typedef struct {
    CANDEV   cdev;
    drv_t   *drv;
    uint32_t mid;       /* TX: MID used by plain write(); RX: acceptance value */
    uint32_t mfilter;   /* RX: bits of mid that must match (0 = accept all) */
    bool     report;    /* hand back to libcan from event_handler() */
} mbox_t;

struct drv {
    /* options */
    unsigned pci_index, core, unit, nrx, ntx, msgq, waitq;
    int      verbosity;
    bool     fd, brs, fd_always, autobus;
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
    unsigned        ver_major, ver_minor;

    /* state */
    mbox_t           rx[MAX_RX_DEVS];
    mbox_t           tx[MAX_TX_DEVS];
    unsigned         tx_rr;
    bool             tx_busy;
    bool             in_event;
    int64_t          ts_delta;
    uint32_t         fault;
    uint32_t         late_events;   /* events caught by the post-unmask re-check */
    uint32_t         events;        /* event_handler() entries for our pulse code */
    CAN_DEVCTL_STATS stats;
};

static drv_t g_drv;

static uint32_t now_us(const drv_t *d)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000 + d->ts_delta);
}

/* ---- transmit ---------------------------------------------------------- */

static void tx_kick(drv_t *d)
{
    if (d->tx_busy) {
        return;
    }
    for (unsigned n = 0; n < d->ntx; n++) {
        unsigned i = (d->tx_rr + n) % d->ntx;
        mbox_t *t = &d->tx[i];
        canmsg_t *e = canmsg_dequeue_element(t->cdev.msg_queue);

        if (e == NULL) {
            continue;
        }
        CAN_MSG *m = &e->cmsg;
        if (m->mid == CAN_MSG_MID_UNKNOWN) {
            /* plain write(): the mailbox MID applies */
            m->mid = t->mid & IDW_EXT_MASK;
            m->ext.is_extended_mid = (t->mid & ~IDW_STD_MASK & IDW_EXT_MASK) != 0;
        }
        bool fdf = d->fd && (d->fd_always || m->len > CAN_MSG_DATA_MAX_CAN);
        int rc = ctucan_hw_tx(&d->hw, TXBUF, m, fdf, fdf && d->brs);
        if (rc != EOK) {
            can_slogf(SLOG2_ERROR, DRV_NAME ": tx%u: dropped mid 0x%08x len %u: %s", t->cdev.dev_unit,
                      m->mid, m->len, strerror(rc));
        }
        canmsg_queue_element(t->cdev.free_queue, e);
        d->tx_rr = (i + 1) % d->ntx;
        if (rc == EOK) {
            d->tx_busy = true;
            return;
        }
    }
}

static void tx_done(drv_t *d)
{
    if (!d->tx_busy) {
        return;
    }
    switch (TXT_STATE(ctucan_rd(&d->hw, CTUCAN_TX_STATUS), TXBUF)) {
    case TXT_TOK:
        d->stats.transmitted_frames++;
        break;
    case TXT_ERR:   /* retransmission limit reached */
    case TXT_ABT:
        d->stats.total_frame_errors++;
        break;
    default:        /* still ready, transmitting or aborting */
        return;
    }
    ctucan_wr(&d->hw, CTUCAN_TX_COMMAND, TXC_TXCE | TXC_TXB(TXBUF));
    d->tx_busy = false;
    for (unsigned i = 0; i < d->ntx; i++) {
        d->tx[i].report = true;     /* lets libcan unblock writers waiting for queue space */
    }
    tx_kick(d);
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

        if (((m->mid ^ r->mid) & r->mfilter) != 0) {
            continue;
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
        r->report = true;
    }
}

static void rx_drain(drv_t *d)
{
    for (unsigned n = 0; n < RX_BUDGET && ctucan_hw_rx_frames(&d->hw) > 0; n++) {
        CAN_MSG m;
        ctucan_flags_t fl;

        if (ctucan_hw_rx(&d->hw, &m, &fl) != EOK) {
            d->stats.total_frame_errors++;
            continue;
        }
        m.ext.timestamp = now_us(d);
        d->stats.received_frames++;
        rx_deliver(d, &m);
    }
}

/* ---- errors ------------------------------------------------------------ */

static void error_event(drv_t *d, uint32_t st)
{
    uint32_t fs = ctucan_rd(&d->hw, CTUCAN_EWL_ERP_FAULT);
    uint32_t cnt = ctucan_rd(&d->hw, CTUCAN_REC_TEC);

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
    if ((st & (INT_FCSI | INT_EWLI)) && fs != d->fault) {
        if (fs & FS_BOF) {
            d->stats.bus_off_state_count++;
            can_slogf(SLOG2_WARNING, DRV_NAME ": bus-off (TEC %u REC %u)%s", (cnt >> 16) & 0x1ffu,
                      cnt & 0x1ffu, d->autobus ? ", requesting recovery" : "");
            if (d->autobus) {
                ctucan_wr(&d->hw, CTUCAN_COMMAND, CMD_ERCRST);
            }
        } else if (fs & FS_ERP) {
            d->stats.error_passive_state_count++;
        } else if (st & INT_EWLI) {
            d->stats.error_warning_state_count++;
        }
        d->fault = fs;
    }
}

/* ---- interrupt service ------------------------------------------------- */

static void service(drv_t *d)
{
    for (int pass = 0; pass < 16; pass++) {
        uint32_t st = ctucan_rd(&d->hw, CTUCAN_INT_STAT) & d->int_ena;

        if (st == 0) {
            break;      /* nothing (more) from us; the line may be shared */
        }
        /* Ack before handling, so events raised while we work (a new TX
         * completing, a new frame arriving) stay pending. */
        ctucan_wr(&d->hw, CTUCAN_INT_STAT, st);
        d->stats.total_interrupts++;

        if (st & (INT_RBNEI | INT_DOI)) {
            d->stats.rx_interrupts++;
            rx_drain(d);
            if (st & INT_DOI) {
                d->stats.hw_receive_overflows++;
                ctucan_wr(&d->hw, CTUCAN_COMMAND, CMD_CDO);
            }
            /* RBNEI re-latches while the FIFO is not empty; clear it again
             * now that it is drained. */
            ctucan_wr(&d->hw, CTUCAN_INT_STAT, INT_RBNEI | INT_RXI);
        }
        if (st & INT_TXBHCI) {
            d->stats.tx_interrupts++;
            tx_done(d);
        }
        if (st & (INT_EWLI | INT_FCSI | INT_BEI)) {
            error_event(d, st);
        }
    }
}

static CANDEV *next_report(drv_t *d)
{
    for (unsigned i = 0; i < d->nrx; i++) {
        if (d->rx[i].report) {
            d->rx[i].report = false;
            return &d->rx[i].cdev;
        }
    }
    for (unsigned i = 0; i < d->ntx; i++) {
        if (d->tx[i].report) {
            d->tx[i].report = false;
            return &d->tx[i].cdev;
        }
    }
    return NULL;
}

static CANDEV *cb_event(void *const hdl, const int code)
{
    drv_t *d = hdl;
    CANDEV *c;

    if (code != PULSE_CODE_IRQ) {
        can_slogf(SLOG2_WARNING, DRV_NAME ": unexpected pulse code %d", code);
        return NULL;
    }
    d->events++;
    if (!d->in_event) {
        d->in_event = true;
        service(d);
    }
    for (;;) {
        /* libcan calls back until NULL: return each mailbox with news once. */
        if ((c = next_report(d)) != NULL) {
            return c;
        }
        InterruptUnmask(d->irq, d->iid);
        /* An event that arrived after the last status read found the line
         * masked. On this route (IRQ 10 in QEMU) no second interrupt comes
         * for a line that is already high, so the FIFO would fill and stall.
         * Check again now that the line is unmasked; anything later raises
         * a fresh interrupt. */
        if ((ctucan_rd(&d->hw, CTUCAN_INT_STAT) & d->int_ena) == 0) {
            break;
        }
        InterruptMask(d->irq, d->iid);
        d->late_events++;
        service(d);
    }
    d->in_event = false;
    return NULL;
}

/* ---- devctl ------------------------------------------------------------ */

static void restart_controller(drv_t *d)
{
    ctucan_hw_stop(&d->hw);
    d->tx_busy = false;
    ctucan_hw_start(&d->hw, &d->cfg, d->int_ena);
    tx_kick(d);
}

static int set_timing(drv_t *d, const CAN_DEVCTL_TIMING *t)
{
    unsigned ts1 = t->time_segment_1, ts2 = t->time_segment_2;
    ctucan_bt_t bt;

    if (t->bit_rate_prescaler == 0 || t->bit_rate_prescaler > 255 || ts1 < 2 || ts1 > 190 ||
        ts2 < 1 || ts2 > 63 || t->sync_jump_width < 1 || t->sync_jump_width > 31) {
        return EINVAL;
    }
    if (t->ref_clock_freq != 0) {
        d->clk_hz = t->ref_clock_freq;
    }
    bt.brp = t->bit_rate_prescaler;
    bt.ph1 = (uint8_t)(ts1 / 2 > 63 ? 63 : ts1 / 2);
    bt.prop = (uint8_t)(ts1 - bt.ph1);
    bt.ph2 = (uint8_t)ts2;
    bt.sjw = t->sync_jump_width;
    d->cfg.nominal = bt;
    d->bitrate = d->clk_hz / (bt.brp * (1u + ts1 + ts2));
    restart_controller(d);
    can_slogf(SLOG2_INFO, DRV_NAME ": timing brp %u ts1 %u ts2 %u sjw %u -> %u bit/s", bt.brp, ts1, ts2,
              bt.sjw, d->bitrate);
    return EOK;
}

static void dump_regs(const drv_t *d)
{
    static const struct { const char *name; uint32_t off; } regs[] = {
        { "DEVICE_ID", CTUCAN_DEVICE_ID }, { "MODE", CTUCAN_MODE }, { "STATUS", CTUCAN_STATUS },
        { "INT_STAT", CTUCAN_INT_STAT }, { "INT_ENA", CTUCAN_INT_ENA_SET }, { "BTR", CTUCAN_BTR },
        { "BTR_FD", CTUCAN_BTR_FD }, { "FAULT", CTUCAN_EWL_ERP_FAULT }, { "REC_TEC", CTUCAN_REC_TEC },
        { "RX_MEM_INFO", CTUCAN_RX_MEM_INFO }, { "RX_STATUS", CTUCAN_RX_STATUS },
        { "TX_STATUS", CTUCAN_TX_STATUS }, { "RX_FR_CTR", CTUCAN_RX_FR_CTR }, { "TX_FR_CTR", CTUCAN_TX_FR_CTR },
    };
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        can_slogf(SLOG2_INFO, DRV_NAME ": %-12s 0x%08x", regs[i].name, ctucan_rd(&d->hw, regs[i].off));
    }
    can_slogf(SLOG2_INFO, DRV_NAME ": events %u, late events %u, tx_busy %d", d->events, d->late_events,
              d->tx_busy);
}

static int cb_devctl(CANDEV *cdev, io_devctl_t *msg)
{
    mbox_t *mb = container_of(cdev, mbox_t, cdev);
    drv_t *d = mb->drv;
    CAN_DCMD_DATA *data = _IO_INPUT_PAYLOAD(msg);
    bool is_rx = cdev->devtype == CANDEV_TYPE_RX;
    int rc;

    switch (msg->i.dcmd) {
    case CAN_DEVCTL_GET_MID:
        data->mid = mb->mid;
        rc = sizeof(data->mid);
        break;
    case CAN_DEVCTL_SET_MID:
        mb->mid = data->mid;
        rc = EOK;
        break;
    case CAN_DEVCTL_GET_MFILTER:
        data->mfilter = mb->mfilter;
        rc = is_rx ? (int)sizeof(data->mfilter) : -EINVAL;
        break;
    case CAN_DEVCTL_SET_MFILTER:
        if (is_rx) {
            mb->mfilter = data->mfilter;
        }
        rc = is_rx ? EOK : -EINVAL;
        break;
    case CAN_DEVCTL_GET_TIMESTAMP:
        data->timestamp = now_us(d);
        rc = sizeof(data->timestamp);
        break;
    case CAN_DEVCTL_SET_TIMESTAMP:
        d->ts_delta += (int64_t)data->timestamp - (int64_t)now_us(d);
        rc = EOK;
        break;
    case CAN_DEVCTL_GET_INFO: {
        CAN_DEVCTL_INFO *in = &data->info;
        memset(in, 0, sizeof(*in));
        snprintf(in->description, sizeof(in->description), "CTU CAN FD %u.%u PCI %04x:%04x core %u%s",
                 d->ver_major, d->ver_minor, CTUCAN_PCI_VENDOR, CTUCAN_PCI_DEVICE, d->core,
                 d->fd ? " FD" : "");
        in->msgq_size = d->msgq;
        in->waitq_size = d->waitq;
        in->mode = CANDEV_MODE_IO;
        in->bit_rate = d->bitrate;
        in->num_tx_mboxes = d->ntx;
        in->num_rx_mboxes = d->nrx;
        in->loopback_internal = d->cfg.loopback;
        in->autobus_on = d->autobus;
        in->silent = d->cfg.listen_only;
        in->bit_rate_prescaler = d->cfg.nominal.brp;
        in->sync_jump_width = d->cfg.nominal.sjw;
        in->time_segment_1 = (uint8_t)(d->cfg.nominal.prop + d->cfg.nominal.ph1);
        in->time_segment_2 = d->cfg.nominal.ph2;
        rc = sizeof(*in);
        break;
    }
    case CAN_DEVCTL_GET_STATS:
        data->stats = d->stats;
        rc = sizeof(data->stats);
        break;
    case CAN_DEVCTL_ERROR:
        data->error.drvr1 = ctucan_rd(&d->hw, CTUCAN_EWL_ERP_FAULT);
        data->error.drvr2 = ctucan_rd(&d->hw, CTUCAN_REC_TEC);
        data->error.drvr3 = ctucan_rd(&d->hw, CTUCAN_ERR_NORM_FD);
        data->error.drvr4 = ctucan_rd(&d->hw, CTUCAN_STATUS);
        rc = sizeof(data->error);
        break;
    case CAN_DEVCTL_SET_TIMING:
        rc = -set_timing(d, &data->timing);
        break;
    case CAN_DEVCTL_DEBUG_INFO:
    case CAN_DEVCTL_DEBUG_INFO2:
        dump_regs(d);
        rc = EOK;
        break;
    case CAN_DEVCTL_WRITE_CANMSG_EXT:
    case CAN_DEVCTL_TX_FRAME_RAW:
        if (is_rx || data->canmsg.len > (d->fd ? CAN_MSG_DATA_MAX_CANFD : CAN_MSG_DATA_MAX_CAN)) {
            rc = -EINVAL;
        } else {
            rc = -ENOTSUP;  /* libcan queues it and calls transmit() */
        }
        break;
    case CAN_DEVCTL_READ_CANMSG_EXT:
    case CAN_DEVCTL_RX_FRAME_RAW_BLOCK:
    case CAN_DEVCTL_RX_FRAME_RAW_NOBLOCK:
        rc = is_rx ? -ENOTSUP : -EINVAL;
        break;
    default:
        rc = -ENOTSUP;
        break;
    }
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

/* ---- setup ------------------------------------------------------------- */

static void usage(void)
{
    printf("usage: " DRV_NAME " [options]\n"
           "  -i idx    PCI device index among %04x:%04x boards (0)\n"
           "  -c core   CTU CAN FD core on the board (0)\n"
           "  -u unit   creates /dev/can<unit>/rxN and txN (core + 1)\n"
           "  -r n      RX mailboxes (1)       -t n  TX mailboxes (1)\n"
           "  -q n      queue length per mailbox (64)  -w n  blocked clients per mailbox (16)\n"
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
    d->verbosity = SLOG2_INFO;
    d->fd = d->brs = true;
    d->clk_hz = 100000000u;
    d->bitrate = 500000u;
    d->dbitrate = 2000000u;
    d->sp = 875;
    d->dsp = 750;
    d->unit = 0;
    while ((c = getopt(argc, argv, "i:c:u:r:t:q:w:b:d:k:CASlLavh")) != -1) {
        switch (c) {
        case 'i': d->pci_index = strtoul(optarg, NULL, 0); break;
        case 'c': d->core = strtoul(optarg, NULL, 0); break;
        case 'u': d->unit = strtoul(optarg, NULL, 0); break;
        case 'r': d->nrx = strtoul(optarg, NULL, 0); break;
        case 't': d->ntx = strtoul(optarg, NULL, 0); break;
        case 'q': d->msgq = strtoul(optarg, NULL, 0); break;
        case 'w': d->waitq = strtoul(optarg, NULL, 0); break;
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
        d->waitq < 1) {
        fprintf(stderr, DRV_NAME ": bad mailbox or queue count\n");
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

    for (int i = 0; i < nba; i++) {
        if (ba[i].bar_num != 1 || ba[i].type != pci_asType_e_MEM) {
            continue;
        }
        uint64_t off = (uint64_t)d->core * CTUCAN_CORE_STRIDE;
        if (off + CTUCAN_CORE_SPAN > ba[i].size) {
            fprintf(stderr, DRV_NAME ": core %u outside BAR1 (size 0x%llx)\n", d->core,
                    (unsigned long long)ba[i].size);
            return ENODEV;
        }
        d->map = mmap_device_memory(NULL, CTUCAN_CORE_SPAN, PROT_READ | PROT_WRITE | PROT_NOCACHE, 0,
                                    ba[i].addr + off);
        if (d->map == MAP_FAILED) {
            fprintf(stderr, DRV_NAME ": mmap_device_memory: %s\n", strerror(errno));
            return errno;
        }
        d->hw.base = d->map;
        printf(DRV_NAME ": B%u:D%u:F%u BAR1 0x%llx core %u irq %d\n", PCI_BUS(bdf), PCI_DEV(bdf),
               PCI_FUNC(bdf), (unsigned long long)(ba[i].addr + off), d->core, (int)d->irq);
        return EOK;
    }
    fprintf(stderr, DRV_NAME ": BAR1 not found\n");
    return ENODEV;
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

        ci.devtype = rx ? CANDEV_TYPE_RX : CANDEV_TYPE_TX;
        ci.dev_unit = (int)i;
        if (can_resmgr_init_device(&mb->cdev, &ci) != EOK) {
            fprintf(stderr, DRV_NAME ": can_resmgr_init_device %u failed\n", i);
            return EIO;
        }
        mb->cdev.cflags |= d->fd ? CANDEV_CFLAG_CANFD : CANDEV_CFLAG_CAN;
        mb->drv = d;
        mb->mid = rx ? 0 : (0x100u + (i - d->nrx)) << 18;   /* default TX: standard id 0x100 + n */
        mb->mfilter = 0;
    }
    return EOK;
}

int main(int argc, char *argv[])
{
    drv_t *d = &g_drv;
    int rc;

    if ((rc = parse_args(d, argc, argv)) != EOK) {
        return rc;
    }
    /* libpci's x86 module reads config space through ports 0xCF8/0xCFC */
    if (ThreadCtl(_NTO_TCTL_IO, 0) == -1) {
        fprintf(stderr, DRV_NAME ": ThreadCtl(_NTO_TCTL_IO): %s\n", strerror(errno));
        return EPERM;
    }
    if ((rc = pci_open(d)) != EOK) {
        return rc;
    }
    if (ctucan_hw_identify(&d->hw, &d->ver_major, &d->ver_minor) != EOK) {
        fprintf(stderr, DRV_NAME ": DEVICE_ID 0x%08x is not CTU CAN FD\n", ctucan_rd(&d->hw, CTUCAN_DEVICE_ID));
        return ENODEV;
    }
    /* A restarted driver must not inherit TX buffers or IRQs from a dead one. */
    ctucan_hw_reset(&d->hw);
    d->int_ena = INT_RBNEI | INT_DOI | INT_TXBHCI | INT_EWLI | INT_FCSI | INT_BEI;

    if (can_resmgr_init(&g_funcs, d->verbosity) != EOK) {
        fprintf(stderr, DRV_NAME ": can_resmgr_init failed\n");
        return EIO;
    }
    if ((rc = devices_init(d)) != EOK) {
        return rc;
    }
    for (unsigned i = 0; i < d->nrx; i++) {
        can_resmgr_create_device(&d->rx[i].cdev);
    }
    for (unsigned i = 0; i < d->ntx; i++) {
        can_resmgr_create_device(&d->tx[i].cdev);
    }
    d->iid = can_resmgr_attach_intr(d, d->irq, PULSE_CODE_IRQ);
    if (d->iid == -1) {
        fprintf(stderr, DRV_NAME ": can_resmgr_attach_intr(%d): %s\n", (int)d->irq, strerror(errno));
        return EIO;
    }
    ctucan_hw_start(&d->hw, &d->cfg, d->int_ena);

    printf(DRV_NAME ": CTU CAN FD %u.%u, /dev/can%u/rx0..rx%u tx%u..tx%u, %s, %u bit/s (brp %u tq %u)",
           d->ver_major, d->ver_minor, d->unit, d->nrx - 1, d->nrx, d->nrx + d->ntx - 1,
           d->fd ? "CAN FD" : "classic CAN", d->bitrate, d->cfg.nominal.brp,
           1 + d->cfg.nominal.prop + d->cfg.nominal.ph1 + d->cfg.nominal.ph2);
    if (d->fd) {
        printf(", data %u bit/s (brp %u tq %u)%s", d->dbitrate, d->cfg.data.brp,
               1 + d->cfg.data.prop + d->cfg.data.ph1 + d->cfg.data.ph2, d->brs ? " BRS" : "");
    }
    printf("\n");
    fflush(stdout);
    can_slogf(SLOG2_INFO, DRV_NAME ": started on irq %d, unit %u", (int)d->irq, d->unit);

    can_resmgr_start();     /* returns on SIGTERM */

    ctucan_hw_stop(&d->hw);
    can_resmgr_detach_intr(d->iid, PULSE_CODE_IRQ);
    for (unsigned i = 0; i < d->nrx; i++) {
        can_resmgr_destroy_device(&d->rx[i].cdev);
    }
    for (unsigned i = 0; i < d->ntx; i++) {
        can_resmgr_destroy_device(&d->tx[i].cdev);
    }
    can_resmgr_fini();
    munmap_device_memory(d->map, CTUCAN_CORE_SPAN);
    pci_device_detach(d->pci);
    return EOK;
}
