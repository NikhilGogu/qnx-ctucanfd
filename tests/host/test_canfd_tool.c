/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * tools/canfd-test.c on the host: its main() with devctl() answered by a
 * script, so the commands that decide a QNX test run can be tried with
 * replies the real driver gives only by chance (a deferred restart that
 * failed, no sample in the monitor window).
 */
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "check.h"

#define main tool_main
#include "../../tools/canfd-test.c"
#undef main

int MsgSendPulse(int coid, int priority, int code, int value)
{
    (void)coid; (void)priority; (void)code; (void)value;
    return 0;
}

/* what the fake driver answers */
static struct {
    unsigned xstats_calls;
    unsigned fail_xstats_from;      /* XSTATS call number (1 based) that fails; 0 = none */
    uint32_t fail_after_set;        /* restart_fail value seen after SET_TIMING */
    uint32_t state_after_set;       /* state word seen after SET_TIMING */
    int      set_timing_rc;
    unsigned set_timing_calls;
} fk;

int devctl(int fd, int dcmd, void *data, size_t nbytes, int *info)
{
    (void)fd; (void)nbytes; (void)info;
    if (dcmd == CTUCAN_DEVCTL_GET_XSTATS) {
        ctucan_xstats_t *x = data;
        fk.xstats_calls++;
        if (fk.fail_xstats_from != 0 && fk.xstats_calls >= fk.fail_xstats_from) {
            return EIO;
        }
        memset(x, 0, sizeof(*x));
        if (fk.set_timing_calls != 0) {
            x->restart_fail = fk.fail_after_set;
            x->state = fk.state_after_set;
        }
        return EOK;
    }
    if (dcmd == CAN_DEVCTL_SET_TIMING) {
        fk.set_timing_calls++;
        return fk.set_timing_rc;
    }
    if (dcmd == CAN_DEVCTL_GET_STATS) {
        memset(data, 0, sizeof(CAN_DEVCTL_STATS));
        return EOK;
    }
    return ENOTTY;
}

static int run_tool(int argc, char *argv[])
{
    fflush(stdout);
    fflush(stderr);
    int out = dup(STDOUT_FILENO), err = dup(STDERR_FILENO);
    int nul = open("/dev/null", O_WRONLY);
    dup2(nul, STDOUT_FILENO);
    dup2(nul, STDERR_FILENO);
    close(nul);
    int rc = tool_main(argc, argv);
    fflush(stdout);
    fflush(stderr);
    dup2(out, STDOUT_FILENO);
    dup2(err, STDERR_FILENO);
    close(out);
    close(err);
    return rc;
}

static void test_timing_reports_failed_restart(void)
{
    char *argv[] = { "canfd-test", "timing", "/dev/null", "4", "43", "6", "4", NULL };

    /* the deferred restart ran and failed before the first read after SET_TIMING:
     * the pending flag is already clear, restart_fail went up by one */
    memset(&fk, 0, sizeof(fk));
    fk.fail_after_set = 1;
    CHECK(run_tool(7, argv) != 0, "a failed deferred restart was reported as success");

    /* the same reply with no failure counted is a success */
    memset(&fk, 0, sizeof(fk));
    CHECK_EQ(run_tool(7, argv), 0);

    /* the restart is still pending on the first read, then fails */
    memset(&fk, 0, sizeof(fk));
    fk.fail_after_set = 1;
    fk.state_after_set = 1u << 11;
    CHECK(run_tool(7, argv) != 0, "a failed restart seen while pending was reported as success");

    /* the stats read after SET_TIMING fails: the tool cannot say the restart worked */
    memset(&fk, 0, sizeof(fk));
    fk.fail_xstats_from = 2;
    CHECK(run_tool(7, argv) != 0, "a failed stats read after SET_TIMING was reported as success");

    /* the stats read before SET_TIMING fails: no baseline, so no claim either way */
    memset(&fk, 0, sizeof(fk));
    fk.fail_xstats_from = 1;
    fk.fail_after_set = 1;
    CHECK_EQ(run_tool(7, argv), 0);
}

static void test_monitor_no_sample(void)
{
    char *argv0[] = { "canfd-test", "monitor", "/dev/null", "0", NULL };
    char *argv1[] = { "canfd-test", "monitor", "/dev/null", "20", NULL };

    memset(&fk, 0, sizeof(fk));
    CHECK(run_tool(4, argv0) != 0, "monitor with a zero window reported success");
    memset(&fk, 0, sizeof(fk));
    CHECK_EQ(run_tool(4, argv1), 0);
}

int main(void)
{
    test_timing_reports_failed_restart();
    test_monitor_no_sample();
    return check_exit("test_canfd_tool");
}
