/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * ctucan-supervisor: starts dev-can-ctucanfd and starts it again when it dies.
 *
 *   ctucan-supervisor [-p READY_PATH] [-n MAX_RESTARTS] -- DRIVER [driver options]
 *
 * The driver leaves its parent once it is running (the parent's wait()
 * returns while the driver goes on; same pid), so the supervisor cannot wait
 * for it. It asks the process manager for a pulse on every process death
 * (procmgr_event_notify(PROCMGR_EVENT_PROCESS_DEATH)) and checks with
 * kill(pid, 0) whether the driver was the one; a 100 ms timeout covers a
 * missed event. A driver that dies within a second of its start is
 * restarted after a delay that doubles up to 1 s. SIGTERM to the supervisor
 * is passed on to the driver, which then shuts down cleanly.
 */
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/procmgr.h>
#include <sys/stat.h>

#define PULSE_DEATH (_PULSE_CODE_MINAVAIL + 7)

extern char **environ;
static volatile sig_atomic_t g_stop;

static uint64_t now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void logf_(const char *fmt, ...)
{
    struct timespec ts;
    va_list ap;

    clock_gettime(CLOCK_REALTIME, &ts);
    printf("%lld.%06ld ctucan-supervisor: ", (long long)ts.tv_sec, ts.tv_nsec / 1000);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static void on_term(int sig)
{
    (void)sig;
    g_stop = 1;
}

static int alive(pid_t pid)
{
    return kill(pid, 0) == 0 || errno != ESRCH;
}

int main(int argc, char *argv[])
{
    const char *ready = "/dev/can1/tx1";
    unsigned max_restarts = 0, restarts = 0;
    int c;

    while ((c = getopt(argc, argv, "p:n:")) != -1) {
        switch (c) {
        case 'p': ready = optarg; break;
        case 'n': max_restarts = (unsigned)strtoul(optarg, NULL, 0); break;
        default:
            fprintf(stderr, "usage: ctucan-supervisor [-p ready_path] [-n max_restarts] -- driver [args]\n");
            return 1;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "ctucan-supervisor: no driver given\n");
        return 1;
    }
    char **dargv = &argv[optind];

    signal(SIGCHLD, SIG_IGN);   /* no zombies: the driver is watched by pid */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    int chid = ChannelCreate(0);
    int coid = chid == -1 ? -1 : ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    struct sigevent ev;
    bool events = false;
    if (coid != -1) {
        SIGEV_PULSE_INIT(&ev, coid, SIGEV_PULSE_PRIO_INHERIT, PULSE_DEATH, 0);
        if (MsgRegisterEvent(&ev, SYSMGR_COID) == -1) {
            logf_("MsgRegisterEvent: %s", strerror(errno));
        }
        events = procmgr_event_notify(PROCMGR_EVENT_PROCESS_DEATH, &ev) != -1;
    }
    if (!events) {
        logf_("no process death events (%s); polling every 100 ms", strerror(errno));
    }

    unsigned delay_ms = 0;
    while (!g_stop) {
        pid_t pid;
        uint64_t t0 = now_us();
        int rc = posix_spawn(&pid, dargv[0], NULL, NULL, dargv, environ);
        if (rc != 0) {
            logf_("spawn %s: %s", dargv[0], strerror(rc));
            sleep(1);
            continue;
        }
        struct stat st;
        uint64_t tr = 0;
        while (now_us() - t0 < 3000000u && alive(pid)) {
            if (stat(ready, &st) == 0) {
                tr = now_us();
                break;
            }
            usleep(500);
        }
        if (tr != 0) {
            logf_("driver pid %d ready (%s) %.1f ms after spawn", pid, ready, (double)(tr - t0) / 1000.0);
        } else {
            logf_("driver pid %d not ready after %.1f ms", pid, (double)(now_us() - t0) / 1000.0);
        }

        while (!g_stop && alive(pid)) {
            struct _pulse p;
            uint64_t timeout = 100000000u;  /* ns */
            TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &timeout, NULL);
            if (chid != -1) {
                MsgReceivePulse(chid, &p, sizeof(p), NULL);
            } else {
                usleep(100000);
            }
        }
        if (g_stop) {
            kill(pid, SIGTERM);
            for (int i = 0; i < 200 && alive(pid); i++) {
                usleep(10000);
            }
            logf_("stopped; driver pid %d %s", pid, alive(pid) ? "still running" : "exited");
            break;
        }
        uint64_t td = now_us();
        logf_("driver pid %d died %.3f s after spawn", pid, (double)(td - t0) / 1e6);
        if (max_restarts != 0 && ++restarts > max_restarts) {
            logf_("restart limit %u reached", max_restarts);
            return 1;
        }
        if (td - t0 < 1000000u) {
            delay_ms = delay_ms == 0 ? 50 : (delay_ms * 2 > 1000 ? 1000 : delay_ms * 2);
        } else {
            delay_ms = 0;
        }
        if (delay_ms != 0) {
            logf_("restarting in %u ms", delay_ms);
            usleep(delay_ms * 1000u);
        }
    }
    return 0;
}
