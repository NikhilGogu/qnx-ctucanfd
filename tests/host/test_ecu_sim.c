/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * tools/ecu-sim.c on the host: its main() with devctl() answered by a stub
 * and the devices pointed at /dev/null, to try the command line. A bad
 * option must end the program before it sends a frame.
 */
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include "check.h"

#ifdef __APPLE__
/* macOS has no clock_nanosleep(); ecu-sim only uses it with an absolute time */
#define TIMER_ABSTIME 1
static int clock_nanosleep(clockid_t id, int flags, const struct timespec *t, struct timespec *rem)
{
    struct timespec now, d;
    (void)flags;
    (void)rem;
    clock_gettime(id, &now);
    d.tv_sec = t->tv_sec - now.tv_sec;
    d.tv_nsec = t->tv_nsec - now.tv_nsec;
    if (d.tv_nsec < 0) {
        d.tv_nsec += 1000000000L;
        d.tv_sec--;
    }
    return d.tv_sec < 0 ? 0 : nanosleep(&d, NULL);
}
#endif

#define main ecu_main
#include "../../tools/ecu-sim.c"
#undef main

static unsigned frames_written;

int devctl(int fd, int dcmd, void *data, size_t nbytes, int *info)
{
    (void)fd; (void)data; (void)nbytes; (void)info;
    if (dcmd == CAN_DEVCTL_WRITE_CANMSG_EXT) {
        frames_written++;
        return EOK;
    }
    usleep(1000);       /* the diagnostic thread's read: nothing to read */
    return EINTR;
}

/* runs ecu-sim with the options in a; frames_written says whether it got as far as sending */
static int run_ecu(char *const a[], int n)
{
    char *argv[16] = { "ecu-sim", "-t", "/dev/null", "-r", "/dev/null" };
    int argc = 5;

    for (int i = 0; i < n; i++) {
        argv[argc++] = a[i];
    }
    argv[argc] = NULL;
    frames_written = 0;
    g_stop = 0;
    optind = 1;
#ifdef __APPLE__
    optreset = 1;
#endif
    fflush(stdout);
    fflush(stderr);
    int out = dup(STDOUT_FILENO), err = dup(STDERR_FILENO);
    int nul = open("/dev/null", O_WRONLY);
    dup2(nul, STDOUT_FILENO);
    dup2(nul, STDERR_FILENO);
    close(nul);
    int rc = ecu_main(argc, argv);
    fflush(stdout);
    fflush(stderr);
    dup2(out, STDOUT_FILENO);
    dup2(err, STDERR_FILENO);
    close(out);
    close(err);
    return rc;
}

static void test_period(void)
{
    char *zero[] = { "-p", "0", "-n", "3" };
    char *word[] = { "-p", "abc", "-n", "3" };
    char *junk[] = { "-p", "100x", "-n", "3" };
    char *big[] = { "-p", "4294967296", "-n", "3" };
    char *good[] = { "-p", "50000", "-n", "3" };

    CHECK(run_ecu(zero, 4) != 0, "a period of 0 was accepted");
    CHECK_EQ(frames_written, 0);
    CHECK(run_ecu(word, 4) != 0, "a period that is not a number was accepted");
    CHECK_EQ(frames_written, 0);
    CHECK(run_ecu(junk, 4) != 0, "a period with trailing text was accepted");
    CHECK_EQ(frames_written, 0);
    CHECK(run_ecu(big, 4) != 0, "a period above 32 bits was accepted");
    CHECK_EQ(frames_written, 0);
    CHECK_EQ(run_ecu(good, 4), 0);
    CHECK(frames_written > 0, "no frame sent with valid options");
}

int main(void)
{
    test_period();
    return check_exit("test_ecu_sim");
}
