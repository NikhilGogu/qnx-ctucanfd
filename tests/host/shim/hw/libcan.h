/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests only. A stand-in for the CAN DDK names from QNX's
 * <hw/libcan.h> that src/dev-can-ctucanfd.c uses. It is written from how the
 * driver uses them and from the DDK behaviour quoted in docs/DESIGN.md 3.1,
 * not copied from the QNX header; types, values and layouts are not QNX's.
 * tests/host/fake_libcan.c implements the calls. Never put this directory on
 * the include path of a QNX build.
 */
#ifndef CTUCAN_HOST_SHIM_LIBCAN_H
#define CTUCAN_HOST_SHIM_LIBCAN_H

#include <stddef.h>
#include <stdint.h>
#include <sys/can_dcmd.h>
#include <sys/iomsg.h>

typedef struct canmsg {
    CAN_MSG        cmsg;
    struct canmsg *next;
} canmsg_t;

typedef struct {
    canmsg_t *head, *tail;
    unsigned  msgcnt;
} canmsg_list_t;

typedef struct {
    size_t cnt;     /* clients blocked on the device */
} can_wait_list_t;

enum { CANDEV_TYPE_RX = 1, CANDEV_TYPE_TX = 2 };
enum { CANDEV_MODE_IO = 0 };
#define CANDEV_CFLAG_CAN    0x1u
#define CANDEV_CFLAG_CANFD  0x2u

typedef struct {
    int      can_unit, dev_unit, devtype, mode;
    unsigned msgq_size, waitq_size;
} CANDEV_INIT;

typedef struct candev {
    int              can_unit, dev_unit, devtype;
    unsigned         cflags;
    canmsg_list_t   *msg_queue, *free_queue;
    can_wait_list_t *wait_client_queue;
    canmsg_t        *pool;      /* the fake libcan's queue elements */
} CANDEV;

typedef struct {
    void    (*transmit)(CANDEV *cdev);
    int     (*devctl)(CANDEV *cdev, io_devctl_t *msg);
    CANDEV *(*event_handler)(void *const hdl, const int code);
} can_drvr_funcs_t;

canmsg_t *canmsg_dequeue_element(canmsg_list_t *list);
void      canmsg_queue_element(canmsg_list_t *list, canmsg_t *e);

int  can_resmgr_init(can_drvr_funcs_t *funcs, int verbosity);
int  can_resmgr_init_device(CANDEV *cdev, const CANDEV_INIT *ci);
int  can_resmgr_create_device(CANDEV *cdev);
void can_resmgr_destroy_device(CANDEV *cdev);
int  can_resmgr_attach_intr(void *hdl, int irq, int code);
void can_resmgr_detach_intr(int id, int code);
void can_resmgr_start(void);
void can_resmgr_fini(void);
void can_slogf(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif
