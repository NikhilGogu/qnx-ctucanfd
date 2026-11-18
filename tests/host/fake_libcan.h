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
 *
 * It also stands in for CLOCK_MONOTONIC and nanosleep() (driver_harness.h
 * maps the driver's calls here): time is virtual, starts at
 * FAKE_CLOCK_START_NS and moves only when a test or a sleep moves it. And it
 * stands in for the TX wait timer (src/ctucanfd_timer.h): the arm is
 * recorded, and fake_timer_fire() does what the helper thread does on QNX,
 * a CTUCAN_DEVCTL_TIMER to the device the driver named.
 */
#ifndef CTUCAN_HOST_FAKE_LIBCAN_H
#define CTUCAN_HOST_FAKE_LIBCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <hw/libcan.h>

#define FAKE_MAX_DEVS   32u
#define FAKE_RX_LOG     256u
#define FAKE_CLOCK_START_NS 10000000000ull  /* 10 s */

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

    uint64_t  now_ns;           /* the virtual CLOCK_MONOTONIC */
    uint64_t  slept_ns;         /* virtual time passed in nanosleep() */

    bool      timer_up;         /* between ctucan_timer_init() and ctucan_timer_fini() */
    char      timer_path[64];
    int       timer_dcmd;
    uint64_t  timer_at_ms;      /* armed for this now_ms(); 0 = disarmed */
    unsigned  timer_arms;       /* ctucan_timer_arm() calls with a time */
    unsigned  timer_fired;      /* CTUCAN_DEVCTL_TIMER sent by fake_timer_fire() */
    void    (*on_timer_arm)(uint64_t at_ms);    /* observer for test_txw_orders.c, NULL otherwise:
                                                 * every ctucan_timer_arm() while the timer is up */
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

/* The virtual clock. */
int fake_clock_gettime(clockid_t id, struct timespec *ts);
int fake_nanosleep(const struct timespec *req, struct timespec *rem);
void fake_advance_ms(unsigned ms);
uint64_t fake_now_ms(void);

/* If the timer is armed and its time has come: disarms it (one shot) and
 * sends its devctl as the helper thread would. Returns whether it did. */
bool fake_timer_fire(void);

/* Moves the clock to the armed time, if later, and fires. Returns whether
 * the timer was armed. */
bool fake_timer_run(void);

#endif
