/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * QNX implementation of src/ctucanfd_timer.h: a CLOCK_MONOTONIC timer in the
 * driver whose SIGEV_PULSE goes to a helper process, and the helper, which
 * turns each pulse into CTUCAN_DEVCTL_TIMER on the driver's device. The
 * devctl is what brings the expiry onto libcan's thread.
 *
 * Why a process and not a thread: the paths libcan attaches for the driver
 * are not attached with _RESMGR_FLAG_SELF, so an open() of /dev/canU/rx0
 * from inside the driver's own process fails with ENOENT (seen on QNX 8.0,
 * docs/DESIGN.md section 5). libcan gives the driver no channel or dispatch
 * handle to send a pulse to either. Another process can open the path.
 *
 * The helper is forked in ctucan_timer_init(), which main() calls right
 * after parsing its arguments: the process has one thread then and no PCI
 * attachment, mapping, interrupt or resource manager, so the helper inherits
 * none of them. It keeps no driver state and needs no lock. The driver's
 * libcan thread only ever calls timer_settime() here, which does not block,
 * and never sends to the helper, so the two cannot wait on each other.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <devctl.h>
#include <sys/neutrino.h>
#include "ctucanfd_timer.h"

#define TIMER_PULSE     (_PULSE_CODE_MINAVAIL + 1)
#define RETRY_NS        10000000ull     /* 10 ms between opens while the driver is not up yet */

static struct {
    int       coid;
    bool      have_timer;
    timer_t   tid;
} g_tm = { .coid = -1 };

/* The helper process. It answers on the pipe with its channel id (or -1),
 * then sends `dcmd` on `path` for each timer pulse. Until the driver's loop
 * answers the open, pulses are dropped: nothing is being served then. It
 * ends when the driver's connection goes away (_PULSE_CODE_DISCONNECT: the
 * driver called ctucan_timer_fini() or ended), or, while it has no
 * connection yet, when its parent is gone. It writes nothing anywhere. */
static void helper(const char *path, int dcmd, int answer, pid_t parent)
{
    int nul = open("/dev/null", O_RDWR);

    /* let go of the terminal or ssh session at once, as the driver does later */
    if (nul != -1) {
        dup2(nul, STDIN_FILENO);
        dup2(nul, STDOUT_FILENO);
        dup2(nul, STDERR_FILENO);
        if (nul > STDERR_FILENO) {
            close(nul);
        }
    }
    /* out of the starting session too, as the driver leaves it in
     * can_resmgr_start() (procmgr_daemon()); SIGTERM still ends the helper */
    (void)setsid();
    signal(SIGHUP, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    int chid = ChannelCreate(_NTO_CHF_DISCONNECT);
    (void)!write(answer, &chid, sizeof(chid));
    close(answer);
    if (chid == -1) {
        _exit(1);
    }

    int fd = -1;
    for (;;) {
        struct _pulse p;
        uint64_t ns = RETRY_NS;

        if (fd == -1 && (fd = open(path, O_RDWR)) == -1) {
            if (getppid() != parent) {
                break;  /* the driver failed before it connected */
            }
            TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &ns, NULL);
        }
        if (MsgReceivePulse(chid, &p, sizeof(p), NULL) == -1) {
            if (errno == ETIMEDOUT || errno == EINTR) {
                continue;
            }
            break;
        }
        if (p.code == _PULSE_CODE_DISCONNECT) {
            break;      /* the driver is gone or stopped its timer */
        }
        if (p.code == TIMER_PULSE && fd != -1) {
            /* The reply carries nothing, and an error (the loop has ended)
             * changes nothing: the driver acts only on a deadline that has
             * passed or on TXT buffers that are final. */
            (void)devctl(fd, dcmd, NULL, 0, NULL);
        }
    }
    _exit(0);
}

int ctucan_timer_init(const char *path, int dcmd)
{
    struct sigaction sa;
    struct sigevent ev;
    int fds[2], chid = -1, rc;
    pid_t parent = getpid(), pid;

    /* The helper is the driver's only child: no zombie when it ends first. */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sa.sa_flags = SA_NOCLDWAIT;
    sigemptyset(&sa.sa_mask);
    (void)sigaction(SIGCHLD, &sa, NULL);

    if (pipe(fds) == -1) {
        return errno;
    }
    if ((pid = fork()) == -1) {
        rc = errno;
        close(fds[0]);
        close(fds[1]);
        return rc;
    }
    if (pid == 0) {
        close(fds[0]);
        helper(path, dcmd, fds[1], parent);
    }
    close(fds[1]);
    ssize_t n;
    do {
        n = read(fds[0], &chid, sizeof(chid));
    } while (n == -1 && errno == EINTR);
    close(fds[0]);
    if (n != (ssize_t)sizeof(chid) || chid == -1) {
        kill(pid, SIGKILL);
        return EAGAIN;
    }
    g_tm.coid = ConnectAttach(0, pid, chid, _NTO_SIDE_CHANNEL, 0);
    if (g_tm.coid == -1) {
        rc = errno;
        kill(pid, SIGKILL);
        return rc;
    }
    SIGEV_PULSE_INIT(&ev, g_tm.coid, SIGEV_PULSE_PRIO_INHERIT, TIMER_PULSE, 0);
    if (timer_create(CLOCK_MONOTONIC, &ev, &g_tm.tid) == -1) {
        rc = errno;
        ctucan_timer_fini();    /* the helper sees the disconnect and ends */
        return rc;
    }
    g_tm.have_timer = true;
    return EOK;
}

void ctucan_timer_arm(uint64_t at_ms)
{
    struct itimerspec its;

    if (!g_tm.have_timer) {
        return;
    }
    memset(&its, 0, sizeof(its));   /* it_value 0 disarms; no interval: one shot */
    its.it_value.tv_sec = (time_t)(at_ms / 1000u);
    its.it_value.tv_nsec = (long)(at_ms % 1000u) * 1000000L;
    (void)timer_settime(g_tm.tid, at_ms != 0 ? TIMER_ABSTIME : 0, &its, NULL);
}

void ctucan_timer_fini(void)
{
    if (g_tm.have_timer) {
        timer_delete(g_tm.tid);
        g_tm.have_timer = false;
    }
    if (g_tm.coid != -1) {
        ConnectDetach(g_tm.coid);   /* the helper gets _PULSE_CODE_DISCONNECT and ends */
        g_tm.coid = -1;
    }
}
