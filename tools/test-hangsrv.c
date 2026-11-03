/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * ctucan-hangsrv: test helper for ctucan-supervisor. A resource manager that
 * registers PATH (/dev/can1/tx1), accepts opens and never replies to a
 * devctl, so a client's devctl stays blocked until this process dies.
 *
 *   ctucan-hangsrv [PATH]
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>

static int io_devctl_hang(resmgr_context_t *ctp, io_devctl_t *msg, RESMGR_OCB_T *ocb)
{
    (void)ctp;
    (void)msg;
    (void)ocb;
    return _RESMGR_NOREPLY;
}

int main(int argc, char *argv[])
{
    const char *path = argc > 1 ? argv[1] : "/dev/can1/tx1";
    resmgr_connect_funcs_t cf;
    resmgr_io_funcs_t iof;
    resmgr_attr_t ra;
    iofunc_attr_t attr;

    dispatch_t *dpp = dispatch_create();
    if (dpp == NULL) {
        fprintf(stderr, "dispatch_create: %s\n", strerror(errno));
        return 1;
    }
    memset(&ra, 0, sizeof(ra));
    ra.nparts_max = 1;
    ra.msg_max_size = 2048;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &cf, _RESMGR_IO_NFUNCS, &iof);
    iof.devctl = io_devctl_hang;
    iofunc_attr_init(&attr, S_IFNAM | 0666, NULL, NULL);
    if (resmgr_attach(dpp, &ra, path, _FTYPE_ANY, 0, &cf, &iof, &attr) == -1) {
        fprintf(stderr, "resmgr_attach %s: %s\n", path, strerror(errno));
        return 1;
    }
    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    while ((ctp = dispatch_block(ctp)) != NULL) {
        dispatch_handler(ctp);
    }
    return 1;
}
