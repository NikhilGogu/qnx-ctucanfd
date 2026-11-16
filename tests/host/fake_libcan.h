/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * A stand-in for libcan, the PCI server and the few kernel calls the driver
 * makes, so src/dev-can-ctucanfd.c runs on the host against the fake core.
 * It follows the DDK behaviour quoted in docs/DESIGN.md 3.1: the event
 * handler is called until it returns NULL; a returned TX device gets
 * transmit(), a returned RX device wakes the reader blocked longest. It is
 * not libcan, and nothing here says how libcan schedules pulses or clients.
 *
 * fake_run_driver() runs the driver's main(); can_resmgr_start() then runs
 * the test body while the driver is up, and returning from it stands for
 * the SIGTERM that ends the driver.
 */
#ifndef CTUCAN_HOST_FAKE_LIBCAN_H
#define CTUCAN_HOST_FAKE_LIBCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <hw/libcan.h>

#define FAKE_MAX_DEVS   32u
#define FAKE_RX_LOG     256u

typedef struct {
    unsigned dev;               /* dev_unit of the mailbox the reader blocked on */
    CAN_MSG  msg;
} fake_rx_t;

typedef struct {
    can_drvr_funcs_t *funcs;
    void     *hdl;              /* what the driver passed to can_resmgr_attach_intr() */
    int       code;             /* its interrupt pulse code */
    bool      intr_attached, resmgr_up, started;
    CANDEV   *dev[FAKE_MAX_DEVS];   /* by dev_unit, as created */
    unsigned  ndev;

    unsigned  unmasks;          /* InterruptUnmask() calls */
    int       unmask_rc;        /* what InterruptUnmask() returns */
    uint32_t  ena_at_unmask;    /* the core's INT_ENA at the last InterruptUnmask() */
    bool      ena_off_at_every_unmask;

    bool      nowake;           /* returned RX devices wake nobody */
    unsigned  returns;          /* devices returned by the event handler */
    unsigned  wakes;            /* readers woken */
    fake_rx_t rx[FAKE_RX_LOG];  /* what the woken readers got, in order */
    unsigned  nrx;

    unsigned  slog_lines;
    char      last_log[256];
} fake_libcan_t;

extern fake_libcan_t lc;

/* Runs drv_main with `args` (split at spaces) and `body` while it is up.
 * Returns what drv_main returned. stdout and stderr are quiet while the
 * driver starts and stops, and open while `body` runs. */
int fake_run_driver(int (*drv_main)(int, char **), const char *args, void (*body)(void));

/* One interrupt pulse as libcan handles it. Returns the devices the driver
 * returned. */
unsigned fake_pulse(void);

/* A client's write() of one frame to a TX device: queued, then transmit().
 * EAGAIN when the queue is full. */
int fake_write(unsigned dev, const CAN_MSG *m);

/* A client's devctl. `in` bytes go to the driver, up to `out` bytes come
 * back into `data`. Returns the handler's value; errno is set when it is -1. */
int fake_devctl(unsigned dev, int dcmd, void *data, size_t in, size_t out);

/* n more clients blocked reading `dev`. */
void fake_block_readers(unsigned dev, unsigned n);

#endif
