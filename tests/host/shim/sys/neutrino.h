/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests only. A stand-in for the kernel calls from QNX's
 * <sys/neutrino.h> that src/dev-can-ctucanfd.c and tools/ctucan-supervisor.c
 * use. It is written from how they use them, not copied from the QNX header;
 * values and layouts are not QNX's. tests/host/fake_libcan.c and
 * tests/host/test_supervisor.c implement the calls. Never put this directory
 * on the include path of a QNX build.
 */
#ifndef CTUCAN_HOST_SHIM_NEUTRINO_H
#define CTUCAN_HOST_SHIM_NEUTRINO_H

#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>

#define _PULSE_CODE_MINAVAIL    0
#define _NTO_TCTL_IO_LEVEL      1
#define _NTO_IO_LEVEL_1         1
#define _NTO_SIDE_CHANNEL       0x40000000
#define _NTO_TIMEOUT_RECEIVE    (1 << 2)
#define SYSMGR_COID             0x40000000
#define SIGEV_PULSE_PRIO_INHERIT (-1)

#define SIGEV_PULSE_INIT(ev, coid, prio, code, value) memset((ev), 0, sizeof(*(ev)))

struct _pulse {
    uint16_t type, subtype;
    int8_t   code;
    int      value;
};

struct _server_info {
    pid_t pid;
};

int InterruptUnmask(int intr, int id);
int ThreadCtl(int cmd, void *data);
int ChannelCreate(unsigned flags);
int ConnectAttach(uint32_t nd, pid_t pid, int chid, unsigned index, int flags);
int ConnectServerInfo(pid_t pid, int coid, struct _server_info *info);
int MsgRegisterEvent(struct sigevent *ev, int coid);
int MsgReceivePulse(int chid, void *pulse, size_t bytes, void *info);
int TimerTimeout(clockid_t id, int flags, const struct sigevent *notify, const uint64_t *ntime, uint64_t *otime);

#endif
