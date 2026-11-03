/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * canfd-test: send and receive CAN / CAN FD frames through any libcan device
 * with the extended-message devctls, which carry the full 64-byte payload.
 *
 *   canfd-test send DEV ID LEN [HEXDATA]   ID in hex; suffix x = 29-bit, r = remote frame
 *   canfd-test write DEV HEXDATA           plain write(): the mailbox MID applies
 *   canfd-test recv DEV [COUNT]            print frames with their full payload
 *   canfd-test seqsend DEV ID LEN COUNT GAP_US   sequenced frames (see below)
 *   canfd-test seqrecv DEV COUNT TIMEOUT_MS      check sequenced frames from the host
 *   canfd-test info DEV | xstats DEV | recover DEV
 *   canfd-test timing DEV BRP TS1 TS2 SJW
 *   canfd-test fi DEV SCENARIO [ARG ARG2 BUDGET FLAGS]   fault injection builds only
 *   canfd-test fitick DEV MS               drive a fault-injection script for MS ms
 *   canfd-test monitor DEV MS              devctls back to back while traffic runs
 *   canfd-test pulse DEV CODE              send a pulse to the driver's channel
 *   canfd-test badsize DEV                 devctls with short payloads (expect EINVAL)
 *
 * Sequenced frames: bytes 0..3 hold a little-endian sequence number, byte i
 * (i >= 4) holds (seq + i) & 0xff. Both seqsend and the host test scripts use
 * this layout, so every byte of every frame is checked.
 */
#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/can_dcmd.h>
#include <sys/neutrino.h>
#include "../src/ctucanfd_devctl.h"

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex(const char *h, uint8_t *out, unsigned max)
{
    unsigned n = 0;

    while (h[0] != '\0' && h[1] != '\0' && n < max) {
        int hi = hexval(h[0]), lo = hexval(h[1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[n++] = (uint8_t)(hi << 4 | lo);
        h += 2;
    }
    return h[0] == '\0' ? (int)n : -1;
}

static void print_msg(const char *dev, const CAN_MSG *m)
{
    if (m->ext.is_extended_mid) {
        printf("%s  %08X  [%02u] ", dev, m->mid & 0x1fffffffu, m->len);
    } else {
        printf("%s  %03X  [%02u] ", dev, (m->mid >> 18) & 0x7ffu, m->len);
    }
    if (!m->ext.is_remote_frame) {
        for (unsigned i = 0; i < m->len; i++) {
            printf(" %02X", m->dat[i]);
        }
    }
    printf("%s  ts=%u\n", m->ext.is_remote_frame ? " RTR" : "", m->ext.timestamp);
    fflush(stdout);
}

static int parse_id(const char *s, CAN_MSG *m)
{
    size_t n = strlen(s);
    int ext = 0;
    char *end;

    /* trailing flags first, so "0x" is id 0 with the extended flag */
    for (; n > 0 && strchr("xXrR", s[n - 1]) != NULL; n--) {
        if (s[n - 1] == 'x' || s[n - 1] == 'X') {
            ext = 1;
        } else {
            m->ext.is_remote_frame = 1;
        }
    }
    char digits[16];
    if (n == 0 || n >= sizeof(digits) || strspn(s, "0123456789abcdefABCDEF") < n) {
        return EINVAL;
    }
    memcpy(digits, s, n);
    digits[n] = '\0';
    unsigned long id = strtoul(digits, &end, 16);
    if (*end != '\0') {
        return EINVAL;
    }
    if (id > 0x7ff) {
        ext = 1;
    }
    m->mid = ext ? (uint32_t)(id & 0x1fffffffu) : (uint32_t)((id & 0x7ffu) << 18);
    m->ext.is_extended_mid = (uint32_t)ext;
    return EOK;
}

static int write_msg(int fd, const CAN_MSG *m)
{
    return devctl(fd, CAN_DEVCTL_WRITE_CANMSG_EXT, (void *)m, sizeof(*m), NULL);
}

static int do_send(int fd, int argc, char *argv[])
{
    CAN_MSG m;

    if (argc < 5) {
        return EINVAL;
    }
    memset(&m, 0, sizeof(m));
    if (parse_id(argv[3], &m) != EOK) {
        return EINVAL;
    }
    unsigned len = (unsigned)strtoul(argv[4], NULL, 0);
    if (len > CAN_MSG_DATA_MAX) {
        return EINVAL;
    }
    m.len = (uint8_t)len;
    for (unsigned i = 0; i < len; i++) {
        m.dat[i] = (uint8_t)i;
    }
    if (argc > 5 && parse_hex(argv[5], m.dat, len) < 0) {
        return EINVAL;
    }
    int rc = write_msg(fd, &m);
    if (rc != EOK) {
        fprintf(stderr, "CAN_DEVCTL_WRITE_CANMSG_EXT: %s\n", strerror(rc));
        return rc;
    }
    print_msg("sent", &m);
    return EOK;
}

static int do_write(int fd, const char *hex)
{
    uint8_t buf[CAN_MSG_DATA_MAX];
    int n = parse_hex(hex, buf, sizeof(buf));

    if (n < 0) {
        return EINVAL;
    }
    if (write(fd, buf, (size_t)n) != n) {
        fprintf(stderr, "write: %s\n", strerror(errno));
        return errno;
    }
    printf("wrote %d byte(s)\n", n);
    return EOK;
}

static int do_recv(int fd, const char *dev, int count)
{
    for (int n = 0; count <= 0 || n < count; n++) {
        CAN_MSG m;
        memset(&m, 0, sizeof(m));
        int rc = devctl(fd, CAN_DEVCTL_READ_CANMSG_EXT, &m, sizeof(m), NULL);
        if (rc != EOK) {
            fprintf(stderr, "CAN_DEVCTL_READ_CANMSG_EXT: %s\n", strerror(rc));
            return rc;
        }
        print_msg(dev, &m);
    }
    return EOK;
}

static void seq_fill(CAN_MSG *m, uint32_t seq, unsigned len)
{
    for (unsigned i = 0; i < len; i++) {
        m->dat[i] = i < 4 ? (uint8_t)(seq >> (8 * i)) : (uint8_t)(seq + i);
    }
}

static uint64_t mono_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* Sends COUNT sequenced frames GAP_US apart; EAGAIN (queue full) is retried. */
static int do_seqsend(int fd, int argc, char *argv[])
{
    CAN_MSG m;

    if (argc < 7) {
        return EINVAL;
    }
    memset(&m, 0, sizeof(m));
    if (parse_id(argv[3], &m) != EOK) {
        return EINVAL;
    }
    unsigned len = (unsigned)strtoul(argv[4], NULL, 0);
    unsigned count = (unsigned)strtoul(argv[5], NULL, 0);
    unsigned gap = (unsigned)strtoul(argv[6], NULL, 0);
    unsigned long retries = 0;
    if (len < 4 || len > CAN_MSG_DATA_MAX) {
        return EINVAL;
    }
    m.len = (uint8_t)len;
    uint64_t t0 = mono_us(), next = t0;
    for (uint32_t seq = 0; seq < count; seq++) {
        seq_fill(&m, seq, len);
        int rc;
        while ((rc = write_msg(fd, &m)) == EAGAIN) {
            retries++;
            usleep(200);
        }
        if (rc != EOK) {
            fprintf(stderr, "seqsend: frame %u: %s\n", seq, strerror(rc));
            return rc;
        }
        if (gap != 0) {
            next += gap;
            uint64_t now = mono_us();
            if (next > now) {
                usleep((useconds_t)(next - now));
            }
        }
    }
    printf("seqsend: %u frame(s) in %llu us, %lu EAGAIN retries\n", count,
           (unsigned long long)(mono_us() - t0), retries);
    return EOK;
}

/* Receives COUNT frames and checks every byte of the sequenced layout. */
static int do_seqrecv(int fd, int argc, char *argv[])
{
    if (argc < 5) {
        return EINVAL;
    }
    unsigned count = (unsigned)strtoul(argv[3], NULL, 0);
    unsigned timeout_ms = (unsigned)strtoul(argv[4], NULL, 0);
    unsigned got = 0, bad = 0, gaps = 0, dups = 0;
    uint32_t expect = 0;
    uint64_t end = mono_us() + (uint64_t)timeout_ms * 1000u;

    while (got < count && mono_us() < end) {
        CAN_MSG m;
        memset(&m, 0, sizeof(m));
        int rc = devctl(fd, CAN_DEVCTL_READ_CANMSG_EXT, &m, sizeof(m), NULL);
        if (rc != EOK) {
            fprintf(stderr, "seqrecv: %s\n", strerror(rc));
            break;
        }
        if (m.len < 4) {
            bad++;
            continue;
        }
        uint32_t seq = (uint32_t)m.dat[0] | (uint32_t)m.dat[1] << 8 | (uint32_t)m.dat[2] << 16 |
                       (uint32_t)m.dat[3] << 24;
        for (unsigned i = 4; i < m.len; i++) {
            if (m.dat[i] != (uint8_t)(seq + i)) {
                bad++;
                break;
            }
        }
        if (seq < expect) {
            dups++;
        } else if (seq > expect) {
            gaps += seq - expect;
        }
        expect = seq + 1;
        got++;
    }
    printf("seqrecv: got %u of %u, bad %u, missing %u, out_of_order %u, last %u\n", got, count, bad, gaps, dups,
           expect == 0 ? 0 : expect - 1);
    return got == count && bad == 0 && gaps == 0 && dups == 0 ? EOK : EIO;
}

static int do_info(int fd)
{
    CAN_DEVCTL_INFO in;
    CAN_DEVCTL_STATS st;
    struct can_devctl_error er;     /* CAN_DEVCTL_ERROR is also the dcmd macro */
    int rc;

    if ((rc = devctl(fd, CAN_DEVCTL_GET_INFO, &in, sizeof(in), NULL)) != EOK) {
        fprintf(stderr, "CAN_DEVCTL_GET_INFO: %s\n", strerror(rc));
        return rc;
    }
    printf("description: %s\nbit_rate: %u  brp %u ts1 %u ts2 %u sjw %u\nmailboxes: rx %u tx %u  msgq %u waitq %u\n",
           in.description, in.bit_rate, in.bit_rate_prescaler, in.time_segment_1, in.time_segment_2,
           in.sync_jump_width, in.num_rx_mboxes, in.num_tx_mboxes, in.msgq_size, in.waitq_size);
    if ((rc = devctl(fd, CAN_DEVCTL_GET_STATS, &st, sizeof(st), NULL)) == EOK) {
        printf("stats: tx %u rx %u irq %u (rx %u tx %u) frame_err %u hw_overflow %u swq_full %u "
               "warn %u passive %u bus_off %u\n",
               st.transmitted_frames, st.received_frames, st.total_interrupts, st.rx_interrupts,
               st.tx_interrupts, st.total_frame_errors, st.hw_receive_overflows, st.sw_receive_q_full,
               st.error_warning_state_count, st.error_passive_state_count, st.bus_off_state_count);
    }
    if ((rc = devctl(fd, CAN_DEVCTL_ERROR, &er, sizeof(er), NULL)) == EOK) {
        printf("error: fault 0x%08x rec_tec 0x%08x err_cnt 0x%08x status 0x%08x\n", er.drvr1, er.drvr2,
               er.drvr3, er.drvr4);
    }
    return EOK;
}

static int do_xstats(int fd)
{
    ctucan_xstats_t x;
    int rc = devctl(fd, CTUCAN_DEVCTL_GET_XSTATS, &x, sizeof(x), NULL);

    if (rc != EOK) {
        fprintf(stderr, "CTUCAN_DEVCTL_GET_XSTATS: %s\n", strerror(rc));
        return rc;
    }
    printf("xstats: hw_pulses %u deferred %u unmask_err %u rearm_pending %u spurious %u reports %u "
           "notify_deferred %u rx_max_batch %u rx_flushes %u rx_flushed %u\n",
           x.hw_pulses, x.deferred, x.unmask_err, x.rearm_pending, x.spurious, x.reports, x.notify_deferred,
           x.rx_max_batch, x.rx_flushes, x.rx_flushed);
    printf("xstats: tx_started %u tx_ok %u tx_failed %u tx_aborted %u tx_lost %u tx_rejected %u tx_hw_busy %u "
           "tx_order_err %u tx_quiesce_timeout %u tx_max_inflight %u tx_batches %u tx_abort_forced %u\n",
           x.tx_started, x.tx_ok, x.tx_failed, x.tx_aborted, x.tx_lost, x.tx_rejected, x.tx_hw_busy,
           x.tx_order_err, x.tx_quiesce_timeout, x.tx_max_inflight, x.tx_batches, x.tx_abort_forced);
    printf("xstats: ewl_up %u ewl_down %u busoff_recovered %u restarts %u restart_fail %u fault_state 0x%x "
           "busoff_hold %u ewl %u inflight %u fi_scenario %u fi_step %u\n",
           x.ewl_up, x.ewl_down, x.busoff_recovered, x.restarts, x.restart_fail, (x.state >> 16) & 7u,
           (x.state >> 9) & 1u, (x.state >> 8) & 1u, x.state & 0xffu, x.state >> 28, (x.state >> 24) & 0xfu);
    return EOK;
}

/* Fault-injection builds: one CTUCAN_DEVCTL_FI_TICK per millisecond for MS ms. */
static int do_fitick(int fd, unsigned ms)
{
    uint64_t end = mono_us() + (uint64_t)ms * 1000u;
    unsigned n = 0;

    while (mono_us() < end) {
        int rc = devctl(fd, CTUCAN_DEVCTL_FI_TICK, NULL, 0, NULL);
        if (rc != EOK) {
            fprintf(stderr, "CTUCAN_DEVCTL_FI_TICK: %s\n", strerror(rc));
            return rc;
        }
        n++;
        usleep(1000);
    }
    printf("fitick: %u ticks\n", n);
    return EOK;
}

/* Devctls back to back for MS ms while traffic runs: how often the received
 * count was seen between its first and last value (devctls answered between
 * interrupt batches) and the longest a devctl took. */
static int do_monitor(int fd, unsigned ms)
{
    uint64_t end = mono_us() + (uint64_t)ms * 1000u, worst = 0, t_last = 0;
    unsigned samples = 0, *vals = calloc(200000, sizeof(unsigned));
    CAN_DEVCTL_STATS st;

    if (vals == NULL) {
        return ENOMEM;
    }
    while (mono_us() < end && samples < 200000) {
        uint64_t t = mono_us();
        int rc = devctl(fd, CAN_DEVCTL_GET_STATS, &st, sizeof(st), NULL);
        uint64_t dt = mono_us() - t;
        if (rc != EOK) {
            fprintf(stderr, "CAN_DEVCTL_GET_STATS: %s\n", strerror(rc));
            free(vals);
            return rc;
        }
        vals[samples++] = st.received_frames;
        if (samples > 1 && vals[samples - 1] != vals[samples - 2]) {
            t_last = t;     /* traffic was still moving */
        }
        if (dt > worst && t_last != 0) {
            worst = dt;     /* only devctls during the traffic */
        }
    }
    unsigned first = vals[0], last = vals[samples - 1], between = 0, steps = 0;
    for (unsigned i = 0; i < samples; i++) {
        if (vals[i] > first && vals[i] < last) {
            between++;
        }
        if (i > 0 && vals[i] != vals[i - 1]) {
            steps++;
        }
    }
    printf("monitor: %u devctls, received %u -> %u in %u steps, %u answered while frames were still coming, "
           "longest %llu us\n", samples, first, last, steps, between, (unsigned long long)worst);
    free(vals);
    return EOK;
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "usage: canfd-test send|write|recv|seqsend|seqrecv|info|xstats|recover|timing|fi|pulse DEV ...\n");
        return 1;
    }
    int fd = open(argv[2], O_RDWR);
    if (fd == -1) {
        fprintf(stderr, "open %s: %s\n", argv[2], strerror(errno));
        return 1;
    }
    int rc;
    const char *cmd = argv[1];
    if (strcmp(cmd, "send") == 0) {
        rc = do_send(fd, argc, argv);
    } else if (strcmp(cmd, "write") == 0 && argc > 3) {
        rc = do_write(fd, argv[3]);
    } else if (strcmp(cmd, "recv") == 0) {
        rc = do_recv(fd, argv[2], argc > 3 ? atoi(argv[3]) : 0);
    } else if (strcmp(cmd, "seqsend") == 0) {
        rc = do_seqsend(fd, argc, argv);
    } else if (strcmp(cmd, "seqrecv") == 0) {
        rc = do_seqrecv(fd, argc, argv);
    } else if (strcmp(cmd, "info") == 0) {
        rc = do_info(fd);
    } else if (strcmp(cmd, "xstats") == 0) {
        rc = do_xstats(fd);
    } else if (strcmp(cmd, "recover") == 0) {
        rc = devctl(fd, CTUCAN_DEVCTL_RECOVER, NULL, 0, NULL);
        printf("CTUCAN_DEVCTL_RECOVER: %s\n", strerror(rc));
    } else if (strcmp(cmd, "timing") == 0 && argc > 6) {
        CAN_DEVCTL_TIMING t = { 0, (uint16_t)strtoul(argv[3], NULL, 0), (uint8_t)strtoul(argv[6], NULL, 0),
                                (uint8_t)strtoul(argv[4], NULL, 0), (uint8_t)strtoul(argv[5], NULL, 0) };
        rc = devctl(fd, CAN_DEVCTL_SET_TIMING, &t, sizeof(t), NULL);
        printf("CAN_DEVCTL_SET_TIMING: %s\n", strerror(rc));
    } else if (strcmp(cmd, "fi") == 0 && argc > 3) {
        ctucan_fi_req_t r = { (uint32_t)strtoul(argv[3], NULL, 0),
                              argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 0) : 0,
                              argc > 5 ? (uint32_t)strtoul(argv[5], NULL, 0) : 0,
                              argc > 6 ? (uint32_t)strtoul(argv[6], NULL, 0) : 0,
                              argc > 7 ? (uint32_t)strtoul(argv[7], NULL, 0) : 0 };
        rc = devctl(fd, CTUCAN_DEVCTL_FI, &r, sizeof(r), NULL);
        printf("CTUCAN_DEVCTL_FI: %s\n", strerror(rc));
    } else if (strcmp(cmd, "badsize") == 0) {
        /* each payload is shorter than its command needs */
        CAN_MSG m;
        uint32_t mid = 0x123u << 18;
        CAN_DEVCTL_TIMING t = { 0, 4, 4, 43, 6 };
        memset(&m, 0, sizeof(m));
        m.len = 8;
        int r1 = devctl(fd, CAN_DEVCTL_SET_MID, &mid, 2, NULL);
        int r2 = devctl(fd, CAN_DEVCTL_WRITE_CANMSG_EXT, &m, 40, NULL);
        int r3 = devctl(fd, CAN_DEVCTL_SET_TIMING, &t, 4, NULL);
        printf("SET_MID/2: %s\nWRITE_CANMSG_EXT/40: %s\nSET_TIMING/4: %s\n", strerror(r1), strerror(r2),
               strerror(r3));
        rc = r1 == EINVAL && r2 == EINVAL && r3 == EINVAL ? EOK : EIO;
    } else if (strcmp(cmd, "fitick") == 0 && argc > 3) {
        rc = do_fitick(fd, (unsigned)strtoul(argv[3], NULL, 0));
    } else if (strcmp(cmd, "monitor") == 0 && argc > 3) {
        rc = do_monitor(fd, (unsigned)strtoul(argv[3], NULL, 0));
    } else if (strcmp(cmd, "pulse") == 0 && argc > 3) {
        /* an fd is a connection to the driver's channel, so it carries pulses */
        rc = MsgSendPulse(fd, -1, atoi(argv[3]), 0) == -1 ? errno : EOK;
        printf("MsgSendPulse code %d: %s\n", atoi(argv[3]), strerror(rc));
    } else {
        rc = EINVAL;
    }
    close(fd);
    if (rc == EINVAL) {
        fprintf(stderr, "canfd-test %s: bad arguments\n", cmd);
    }
    return rc == EOK ? 0 : 1;
}
