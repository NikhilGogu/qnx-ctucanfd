/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * tools/ctucan-supervisor.c on the host: its main loop with posix_spawn()
 * and usleep() replaced by test functions, and the QNX calls it makes
 * answered by stubs (no channel, so it takes its polling path). A stop
 * request during the restart delay must start no driver.
 */
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <unistd.h>
#include "check.h"

static unsigned spawns, sleeps;
static useconds_t stop_in_sleep;    /* usleep() of this length gets a SIGTERM */

static int test_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *fa,
                      const posix_spawnattr_t *attr, char *const argv[], char *const envp[])
{
    (void)pid;
    (void)path;
    (void)fa;
    (void)attr;
    (void)argv;
    (void)envp;
    spawns++;
    return ENOENT;  /* every start fails, so each round sleeps first */
}

static int test_usleep(useconds_t us)
{
    sleeps++;
    if (us == stop_in_sleep) {
        raise(SIGTERM);
    }
    return 0;
}

#define posix_spawn test_spawn
#define usleep      test_usleep
#define main        sup_main
#include "../../tools/ctucan-supervisor.c"
#undef main
#undef usleep
#undef posix_spawn

int ChannelCreate(unsigned flags) { (void)flags; errno = ENOSYS; return -1; }
int ConnectAttach(uint32_t nd, pid_t pid, int chid, unsigned index, int flags)
{
    (void)nd; (void)pid; (void)chid; (void)index; (void)flags;
    errno = ENOSYS;
    return -1;
}
/* what the probe's ConnectServerInfo() and devctl() answer: 0 = no QNX (-1, ENOSYS);
 * 1 = a valid connection, the server is the test process and answers GET_INFO;
 * 2 = a connection id that is not the fd, no error (errno 0) */
static int g_csi_mode;

int ConnectServerInfo(pid_t pid, int coid, struct _server_info *info)
{
    (void)pid;
    errno = 0;
    if (g_csi_mode == 1) {
        info->pid = getppid();
        return coid;
    }
    if (g_csi_mode == 2) {
        return coid + 1;
    }
    errno = ENOSYS;
    return -1;
}
int MsgRegisterEvent(struct sigevent *ev, int coid) { (void)ev; (void)coid; return -1; }
int MsgReceivePulse(int chid, void *pulse, size_t bytes, void *info)
{
    (void)chid; (void)pulse; (void)bytes; (void)info;
    return -1;
}
int TimerTimeout(clockid_t id, int flags, const struct sigevent *notify, const uint64_t *ntime, uint64_t *otime)
{
    (void)id; (void)flags; (void)notify; (void)ntime; (void)otime;
    return 0;
}
int procmgr_event_notify(unsigned flags, const struct sigevent *ev) { (void)flags; (void)ev; errno = ENOSYS; return -1; }
int devctl(int fd, int dcmd, void *data, size_t nbytes, int *info)
{
    (void)fd; (void)dcmd; (void)data; (void)nbytes; (void)info;
    return g_csi_mode == 1 ? EOK : ENOSYS;
}

int main(void)
{
    char *argv[] = { "ctucan-supervisor", "--", "/nonexistent/dev-can-ctucanfd", NULL };

    /* the first failed start is followed by a 50 ms delay; SIGTERM arrives in it */
    stop_in_sleep = 50000;
    fflush(stdout);
    int out = dup(STDOUT_FILENO);
    int nul = open("/dev/null", O_WRONLY);  /* the supervisor logs every round */
    dup2(nul, STDOUT_FILENO);
    close(nul);
    int rc = sup_main(4, argv);
    fflush(stdout);
    dup2(out, STDOUT_FILENO);
    close(out);
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);

    /* the probe: a connection id that differs from the fd is no success, whatever errno holds */
    pid_t server = 0;
    g_csi_mode = 1;
    CHECK_EQ(probe("/dev/null", getpid(), 2000, &server), EOK);
    CHECK_EQ(server, getpid());
    g_csi_mode = 2;
    int prc = probe("/dev/null", getpid(), 2000, &server);
    CHECK(prc != EOK, "probe took a connection id that is not the fd for success");
    g_csi_mode = 0;

    CHECK_EQ(rc, 0);
    CHECK_EQ(sleeps, 1);
    CHECK(spawns == 1, "%u driver start(s); a stop during the restart delay must start none", spawns);
    return check_exit("test_supervisor");
}
