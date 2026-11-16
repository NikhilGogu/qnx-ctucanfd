/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <pci/pci.h>
#include "fake_core.h"
#include "fake_libcan.h"

fake_libcan_t lc;

static void (*g_body)(void);
static int g_saved[3] = { -1, -1, -1 };

#define BAR0_ADDR   0x10000000u
#define BAR1_ADDR   0x20000000u
static uint32_t g_bar0[0x1000 / 4];

/* ---- queues ------------------------------------------------------------ */

canmsg_t *canmsg_dequeue_element(canmsg_list_t *list)
{
    canmsg_t *e = list->head;

    if (e != NULL) {
        list->head = e->next;
        if (list->head == NULL) {
            list->tail = NULL;
        }
        e->next = NULL;
        list->msgcnt--;
    }
    return e;
}

void canmsg_queue_element(canmsg_list_t *list, canmsg_t *e)
{
    e->next = NULL;
    if (list->tail != NULL) {
        list->tail->next = e;
    } else {
        list->head = e;
    }
    list->tail = e;
    list->msgcnt++;
}

/* ---- resource manager -------------------------------------------------- */

int can_resmgr_init(can_drvr_funcs_t *funcs, int verbosity)
{
    (void)verbosity;
    lc.funcs = funcs;
    lc.resmgr_up = true;
    return EOK;
}

int can_resmgr_init_device(CANDEV *cdev, const CANDEV_INIT *ci)
{
    memset(cdev, 0, sizeof(*cdev));
    cdev->can_unit = ci->can_unit;
    cdev->dev_unit = ci->dev_unit;
    cdev->devtype = ci->devtype;
    cdev->msg_queue = calloc(1, sizeof(*cdev->msg_queue));
    cdev->free_queue = calloc(1, sizeof(*cdev->free_queue));
    cdev->wait_client_queue = calloc(1, sizeof(*cdev->wait_client_queue));
    cdev->pool = calloc(ci->msgq_size, sizeof(*cdev->pool));
    if (cdev->msg_queue == NULL || cdev->free_queue == NULL || cdev->wait_client_queue == NULL ||
        cdev->pool == NULL) {
        return ENOMEM;
    }
    for (unsigned i = 0; i < ci->msgq_size; i++) {
        canmsg_queue_element(cdev->free_queue, &cdev->pool[i]);
    }
    return EOK;
}

int can_resmgr_create_device(CANDEV *cdev)
{
    if ((unsigned)cdev->dev_unit >= FAKE_MAX_DEVS) {
        errno = EINVAL;
        return -1;
    }
    lc.dev[cdev->dev_unit] = cdev;
    if ((unsigned)cdev->dev_unit + 1 > lc.ndev) {
        lc.ndev = (unsigned)cdev->dev_unit + 1;
    }
    return 0;
}

void can_resmgr_destroy_device(CANDEV *cdev)
{
    if ((unsigned)cdev->dev_unit < FAKE_MAX_DEVS && lc.dev[cdev->dev_unit] == cdev) {
        lc.dev[cdev->dev_unit] = NULL;
    }
    free(cdev->msg_queue);
    free(cdev->free_queue);
    free(cdev->wait_client_queue);
    free(cdev->pool);
    cdev->msg_queue = cdev->free_queue = NULL;
    cdev->wait_client_queue = NULL;
    cdev->pool = NULL;
}

int can_resmgr_attach_intr(void *hdl, int irq, int code)
{
    (void)irq;
    lc.hdl = hdl;
    lc.code = code;
    lc.intr_attached = true;
    return 7;
}

void can_resmgr_detach_intr(int id, int code)
{
    (void)id;
    (void)code;
    lc.intr_attached = false;
}

static void quiet(bool on)
{
    fflush(stdout);
    fflush(stderr);
    if (on) {
        int nul = open("/dev/null", O_RDWR);
        for (int fd = 0; fd < 3; fd++) {
            if (g_saved[fd] == -1) {
                g_saved[fd] = dup(fd);
            }
            dup2(nul, fd);
        }
        close(nul);
    } else {
        for (int fd = 0; fd < 3; fd++) {
            if (g_saved[fd] != -1) {
                dup2(g_saved[fd], fd);
            }
        }
    }
}

void can_resmgr_start(void)
{
    lc.started = true;
    quiet(false);       /* the driver pointed stdio at /dev/null; the test speaks */
    if (g_body != NULL) {
        g_body();
    }
    quiet(true);
}

void can_resmgr_fini(void)
{
    lc.resmgr_up = false;
}

void can_slogf(int level, const char *fmt, ...)
{
    va_list ap;

    (void)level;
    va_start(ap, fmt);
    vsnprintf(lc.last_log, sizeof(lc.last_log), fmt, ap);
    va_end(ap);
    lc.slog_lines++;
    if (getenv("FAKE_SLOG") != NULL) {
        fprintf(stderr, "slog: %s\n", lc.last_log);
    }
}

/* ---- kernel, PCI server and mapping ------------------------------------ */

int InterruptUnmask(int intr, int id)
{
    (void)intr;
    (void)id;
    lc.unmasks++;
    lc.ena_at_unmask = fake.int_ena;
    if (fake.int_ena != 0) {
        lc.ena_off_at_every_unmask = false;
    }
    if (lc.unmask_rc != 0) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

int ThreadCtl(int cmd, void *data)
{
    (void)cmd;
    (void)data;
    return 0;
}

pci_bdf_t pci_device_find(unsigned idx, unsigned vid, unsigned did, unsigned ccode)
{
    (void)ccode;
    return idx == 0 && vid == CTUCAN_PCI_VENDOR && did == CTUCAN_PCI_DEVICE ? 0x0108u : PCI_BDF_NONE;
}

pci_devhdl_t pci_device_attach(pci_bdf_t bdf, pci_attachFlags_e flags, pci_err_t *err)
{
    (void)bdf;
    (void)flags;
    *err = PCI_ERR_OK;
    return (pci_devhdl_t)(void *)g_bar0;
}

pci_err_t pci_device_detach(pci_devhdl_t hdl)
{
    (void)hdl;
    return PCI_ERR_OK;
}

pci_err_t pci_device_read_ba(pci_devhdl_t hdl, int_t *nba, pci_ba_t *ba, pci_reqType_e req)
{
    (void)hdl;
    (void)req;
    if (*nba < 2) {
        return 1;
    }
    ba[0] = (pci_ba_t){ .addr = BAR0_ADDR, .size = 0x1000, .type = pci_asType_e_MEM, .bar_num = 0 };
    ba[1] = (pci_ba_t){ .addr = BAR1_ADDR, .size = CTUCAN_CORE_SPAN, .type = pci_asType_e_MEM, .bar_num = 1 };
    *nba = 2;
    return PCI_ERR_OK;
}

pci_err_t pci_device_read_irq(pci_devhdl_t hdl, int_t *nirq, pci_irq_t *irq)
{
    (void)hdl;
    irq[0] = 10;
    *nirq = 1;
    return PCI_ERR_OK;
}

const char *pci_strerror(pci_err_t err)
{
    return err == PCI_ERR_OK ? "ok" : "fake PCI error";
}

void *mmap_device_memory(void *addr, size_t len, int prot, int flags, uint64_t physical)
{
    (void)addr;
    (void)prot;
    (void)flags;
    if (physical == BAR0_ADDR && len <= sizeof(g_bar0)) {
        g_bar0[CTUCAN_BAR0_ID / 4] = 1;     /* one core on the board */
        return g_bar0;
    }
    if (physical == BAR1_ADDR && len <= sizeof(fake.mem)) {
        return fake.mem;
    }
    errno = ENXIO;
    return MAP_FAILED;
}

int munmap_device_memory(void *addr, size_t len)
{
    (void)addr;
    (void)len;
    return 0;
}

/* ---- the test's side ---------------------------------------------------- */

int fake_run_driver(int (*drv_main)(int, char **), const char *args, void (*body)(void))
{
    char buf[256], *argv[32];
    int argc = 0;
    ctucan_hw_t hw;

    memset(&lc, 0, sizeof(lc));
    lc.ena_off_at_every_unmask = true;
    fake_init(&hw);
    g_body = body;
    argv[argc++] = "dev-can-ctucanfd";
    snprintf(buf, sizeof(buf), "%s", args);
    for (char *t = strtok(buf, " "); t != NULL && argc < 31; t = strtok(NULL, " ")) {
        argv[argc++] = t;
    }
    argv[argc] = NULL;

    quiet(true);
    int rc = drv_main(argc, argv);
    quiet(false);
    for (int fd = 0; fd < 3; fd++) {
        close(g_saved[fd]);
        g_saved[fd] = -1;
    }
    g_body = NULL;
    return rc;
}

unsigned fake_pulse(void)
{
    unsigned n = 0;
    CANDEV *c;

    while ((c = lc.funcs->event_handler(lc.hdl, lc.code)) != NULL) {
        n++;
        lc.returns++;
        if (n > 100000) {
            fprintf(stderr, "fake_pulse: the event handler never returns NULL\n");
            abort();
        }
        if (c->devtype == CANDEV_TYPE_TX) {
            lc.funcs->transmit(c);
        } else if (!lc.nowake && c->wait_client_queue->cnt > 0 && c->msg_queue->msgcnt > 0) {
            canmsg_t *e = canmsg_dequeue_element(c->msg_queue);
            if (lc.nrx < FAKE_RX_LOG) {
                lc.rx[lc.nrx].dev = (unsigned)c->dev_unit;
                lc.rx[lc.nrx].msg = e->cmsg;
                lc.nrx++;
            }
            canmsg_queue_element(c->free_queue, e);
            c->wait_client_queue->cnt--;
            lc.wakes++;
        }
    }
    return n;
}

int fake_write(unsigned dev, const CAN_MSG *m)
{
    CANDEV *c = lc.dev[dev];
    canmsg_t *e = canmsg_dequeue_element(c->free_queue);

    if (e == NULL) {
        return EAGAIN;
    }
    e->cmsg = *m;
    canmsg_queue_element(c->msg_queue, e);
    lc.funcs->transmit(c);
    return EOK;
}

int fake_devctl(unsigned dev, int dcmd, void *data, size_t in, size_t out)
{
    union {
        struct {
            io_devctl_t   hdr;
            CAN_DCMD_DATA payload;
        } m;
        uint8_t raw[1024];
    } u;

    memset(&u, 0xa5, sizeof(u));    /* bytes the client did not send are not zero */
    u.m.hdr.i.dcmd = (uint32_t)dcmd;
    u.m.hdr.i.nbytes = (uint32_t)in;
    if (in > 0) {
        memcpy(&u.m.payload, data, in);
    }
    int rc = lc.funcs->devctl(lc.dev[dev], &u.m.hdr);
    if (out > 0) {
        memcpy(data, &u.m.payload, out);
    }
    return rc;
}

void fake_block_readers(unsigned dev, unsigned n)
{
    lc.dev[dev]->wait_client_queue->cnt += n;
}
