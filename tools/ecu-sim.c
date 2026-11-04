/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * ecu-sim: a small simulated battery-management ECU for QNX on libcan devices.
 *
 *   ecu-sim [-t TXDEV] [-r RXDEV] [-i ID] [-p PERIOD_US] [-P PRIO] [-n COUNT]
 *
 * Telemetry: every PERIOD_US (10 ms) one 64-byte CAN FD frame on ID (0x18F),
 * sent from a SCHED_FIFO thread at PRIO (30) with absolute deadlines:
 *
 *   0..3   sequence number, little endian (one per period, so a gap shows lost periods)
 *   4..7   milliseconds since start
 *   8..9   pack voltage, 0.1 V        10..11  pack current, 0.1 A, signed
 *   12     state of charge, %         13      state of health, %
 *   14..15 highest cell temperature, 0.1 degC, signed
 *   16..47 16 cell voltages, mV
 *   48..59 zero
 *   60..61 times the ECU reopened a device    62  0
 *   63     CRC-8 (polynomial 0x1D, init 0xFF) over bytes 0..62
 *
 * Diagnostics: a classic frame 7DF#02015B (OBD-II service 01, PID 0x5B,
 * "hybrid/EV battery pack remaining life", SAE J1979) is answered with
 * 7E8#03415Bxx0000000000, xx = state of charge * 255 / 100.
 *
 * Echo, for round-trip tests from another node: a data frame on 0x1E0 is
 * sent back unchanged on 0x1E8 (a 64-byte request gives a 64-byte CAN FD reply).
 *
 * When the driver goes away (restart, crash) every devctl fails; each thread
 * then reopens its device every millisecond until the driver is back. A
 * period that is already over when the thread wakes (an overrun, or time
 * spent reopening) is skipped and counted, never caught up.
 */
#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/can_dcmd.h>

static const char *g_tx = "/dev/can1/tx1", *g_rx = "/dev/can1/rx0";
static uint32_t g_id = 0x18f;
static unsigned g_period_us = 10000, g_prio = 30;
static unsigned long g_count;
static atomic_uint g_reopens, g_answers, g_echoes, g_soc = 80;

#define ECHO_REQ 0x1e0u
#define ECHO_RSP 0x1e8u
static volatile sig_atomic_t g_stop;

static uint64_t now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static uint8_t crc8(const uint8_t *p, unsigned n)
{
    uint8_t c = 0xff;

    while (n-- > 0) {
        c ^= *p++;
        for (int b = 0; b < 8; b++) {
            c = (uint8_t)(c & 0x80 ? (c << 1) ^ 0x1d : c << 1);
        }
    }
    return c;
}

static void ts_add_us(struct timespec *ts, uint64_t us)
{
    ts->tv_sec += (time_t)(us / 1000000u);
    ts->tv_nsec += (long)(us % 1000000u) * 1000;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec++;
    }
}

static void put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* Opens dev, retrying every millisecond while the driver is away. */
static int reopen(const char *dev, int fd, int flags, const char *who)
{
    uint64_t t0 = now_us();
    bool again = fd != -1;

    if (again) {
        close(fd);
    }
    while (!g_stop && (fd = open(dev, flags)) == -1) {
        usleep(1000);
    }
    if (fd != -1 && again) {
        atomic_fetch_add(&g_reopens, 1);
        printf("ecu-sim: %s reopened %s after %.1f ms\n", who, dev, (double)(now_us() - t0) / 1000.0);
        fflush(stdout);
    }
    return fd;
}

static void *diag_thread(void *arg)
{
    (void)arg;
    int rfd = reopen(g_rx, -1, O_RDONLY, "diag");
    int tfd = reopen(g_tx, -1, O_WRONLY, "diag");

    while (!g_stop) {
        CAN_MSG m;
        memset(&m, 0, sizeof(m));
        int rc = devctl(rfd, CAN_DEVCTL_READ_CANMSG_EXT, &m, sizeof(m), NULL);
        if (rc != EOK) {
            if (rc != EINTR) {
                rfd = reopen(g_rx, rfd, O_RDONLY, "diag rx");
            }
            continue;
        }
        if (m.ext.is_extended_mid || m.ext.is_remote_frame) {
            continue;
        }
        uint32_t id = (m.mid >> 18) & 0x7ffu;
        atomic_uint *counter;
        CAN_MSG r;
        memset(&r, 0, sizeof(r));
        if (id == ECHO_REQ && m.len <= CAN_MSG_DATA_MAX) {
            r.mid = ECHO_RSP << 18;
            r.len = m.len;
            memcpy(r.dat, m.dat, m.len);
            counter = &g_echoes;
        } else if (id == 0x7df && m.len >= 3 && m.dat[0] == 2 && m.dat[1] == 0x01 && m.dat[2] == 0x5b) {
            r.mid = 0x7e8u << 18;
            r.len = 8;
            r.dat[0] = 3;
            r.dat[1] = 0x41;
            r.dat[2] = 0x5b;
            r.dat[3] = (uint8_t)(atomic_load(&g_soc) * 255u / 100u);
            counter = &g_answers;
        } else {
            continue;
        }
        while (!g_stop && (rc = devctl(tfd, CAN_DEVCTL_WRITE_CANMSG_EXT, &r, sizeof(r), NULL)) != EOK) {
            if (rc == EAGAIN) {
                usleep(200);
            } else {
                tfd = reopen(g_tx, tfd, O_WRONLY, "diag tx");
            }
        }
        atomic_fetch_add(counter, 1);
    }
    return NULL;
}

static void on_term(int sig)
{
    (void)sig;
    g_stop = 1;
}

int main(int argc, char *argv[])
{
    int c;

    while ((c = getopt(argc, argv, "t:r:i:p:P:n:")) != -1) {
        switch (c) {
        case 't': g_tx = optarg; break;
        case 'r': g_rx = optarg; break;
        case 'i': g_id = (uint32_t)strtoul(optarg, NULL, 16); break;
        case 'p': g_period_us = (unsigned)strtoul(optarg, NULL, 0); break;
        case 'P': g_prio = (unsigned)strtoul(optarg, NULL, 0); break;
        case 'n': g_count = strtoul(optarg, NULL, 0); break;
        default:
            fprintf(stderr, "usage: ecu-sim [-t txdev] [-r rxdev] [-i hexid] [-p period_us] [-P prio] [-n count]\n");
            return 1;
        }
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    struct sched_param sp = { .sched_priority = (int)g_prio };
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != EOK) {
        fprintf(stderr, "ecu-sim: SCHED_FIFO %u refused, running at the default priority\n", g_prio);
    }
    pthread_t tid;
    if (pthread_create(&tid, NULL, diag_thread, NULL) != EOK) {
        fprintf(stderr, "ecu-sim: no diagnostic thread\n");
        return 1;
    }
    int fd = reopen(g_tx, -1, O_WRONLY, "telemetry");
    printf("ecu-sim: telemetry id %03X every %u us on %s, diagnostics and echo (%03X -> %03X) on %s\n", g_id,
           g_period_us, g_tx, ECHO_REQ, ECHO_RSP, g_rx);
    fflush(stdout);

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    uint64_t t0 = now_us();
    unsigned long sent = 0, missed = 0;
    unsigned long overruns = 0;
    for (uint32_t seq = 0; !g_stop && (g_count == 0 || seq < g_count); seq++) {
        ts_add_us(&next, g_period_us);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

        /* Periods that are already over are skipped: the
         * sequence counts periods, so the receiver sees what was lost. */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t lag = ((int64_t)now.tv_sec - next.tv_sec) * 1000000 + (now.tv_nsec - next.tv_nsec) / 1000;
        if (lag >= (int64_t)g_period_us) {
            uint32_t skip = (uint32_t)((uint64_t)lag / g_period_us);
            seq += skip;
            missed += skip;
            overruns++;
            ts_add_us(&next, (uint64_t)skip * g_period_us);
        }

        /* a slowly discharging pack with some ripple */
        uint32_t ms = (uint32_t)((now_us() - t0) / 1000u);
        unsigned soc = 80u - (ms / 60000u) % 60u;
        atomic_store(&g_soc, soc);
        CAN_MSG m;
        memset(&m, 0, sizeof(m));
        m.mid = g_id << 18;
        m.len = 64;
        memcpy(m.dat, &seq, 4);
        memcpy(m.dat + 4, &ms, 4);
        put16(m.dat + 8, 3550u + soc + (seq % 7u));
        put16(m.dat + 10, (unsigned)(int16_t)(-125 + (int)(seq % 11u)));
        m.dat[12] = (uint8_t)soc;
        m.dat[13] = 97;
        put16(m.dat + 14, 312u + (seq % 5u));
        for (unsigned i = 0; i < 16; i++) {
            put16(m.dat + 16 + 2 * i, 3700u + soc + ((seq + i * 3u) % 9u));
        }
        put16(m.dat + 60, atomic_load(&g_reopens));
        m.dat[63] = crc8(m.dat, 63);

        int rc = devctl(fd, CAN_DEVCTL_WRITE_CANMSG_EXT, &m, sizeof(m), NULL);
        if (rc == EOK) {
            sent++;
        } else {
            missed++;   /* this period's frame is lost; the sequence moves on */
            if (rc != EAGAIN) {
                fd = reopen(g_tx, fd, O_WRONLY, "telemetry");
            }
        }
    }
    g_stop = 1;
    printf("ecu-sim: sent %lu, missed %lu (overruns %lu), reopens %u, diagnostic answers %u, echoes %u\n", sent,
           missed, overruns, atomic_load(&g_reopens), atomic_load(&g_answers), atomic_load(&g_echoes));
    return 0;
}
