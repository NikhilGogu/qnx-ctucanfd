/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * canfd-test: send and receive CAN / CAN FD frames through any libcan device
 * with the extended-message devctls, which carry the full 64-byte payload.
 *
 *   canfd-test send DEV ID LEN [HEXDATA]   ID in hex; suffix x = 29-bit id
 *   canfd-test recv DEV [COUNT]
 *   canfd-test info DEV
 *   canfd-test pulse DEV CODE     send a pulse to the driver's channel
 */
#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/can_dcmd.h>
#include <sys/neutrino.h>

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void print_msg(const char *dev, const CAN_MSG *m)
{
    if (m->ext.is_extended_mid) {
        printf("%s  %08X  [%02u] ", dev, m->mid & 0x1fffffffu, m->len);
    } else {
        printf("%s  %03X  [%02u] ", dev, (m->mid >> 18) & 0x7ffu, m->len);
    }
    for (unsigned i = 0; i < m->len; i++) {
        printf(" %02X", m->dat[i]);
    }
    printf("%s  ts=%u\n", m->ext.is_remote_frame ? " RTR" : "", m->ext.timestamp);
    fflush(stdout);
}

static int do_send(int fd, int argc, char *argv[])
{
    CAN_MSG m;
    char *end;

    if (argc < 5) {
        return EINVAL;
    }
    memset(&m, 0, sizeof(m));
    unsigned long id = strtoul(argv[3], &end, 16);
    int ext = (*end == 'x' || *end == 'X' || id > 0x7ff);
    m.mid = ext ? (uint32_t)(id & 0x1fffffffu) : (uint32_t)((id & 0x7ffu) << 18);
    m.ext.is_extended_mid = (uint32_t)ext;
    unsigned len = (unsigned)strtoul(argv[4], NULL, 0);
    if (len > CAN_MSG_DATA_MAX) {
        return EINVAL;
    }
    m.len = (uint8_t)len;
    for (unsigned i = 0; i < len; i++) {
        m.dat[i] = (uint8_t)i;
    }
    if (argc > 5) {
        const char *h = argv[5];
        for (unsigned i = 0; i < len && h[0] && h[1]; i++, h += 2) {
            int hi = hexval(h[0]), lo = hexval(h[1]);
            if (hi < 0 || lo < 0) {
                return EINVAL;
            }
            m.dat[i] = (uint8_t)(hi << 4 | lo);
        }
    }
    int rc = devctl(fd, CAN_DEVCTL_WRITE_CANMSG_EXT, &m, sizeof(m), NULL);
    if (rc != EOK) {
        fprintf(stderr, "CAN_DEVCTL_WRITE_CANMSG_EXT: %s\n", strerror(rc));
        return rc;
    }
    print_msg("sent", &m);
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
        printf("stats: tx %u rx %u irq %u (rx %u tx %u) frame_err %u hw_overflow %u swq_full %u bus_off %u\n",
               st.transmitted_frames, st.received_frames, st.total_interrupts, st.rx_interrupts,
               st.tx_interrupts, st.total_frame_errors, st.hw_receive_overflows, st.sw_receive_q_full,
               st.bus_off_state_count);
    }
    if ((rc = devctl(fd, CAN_DEVCTL_ERROR, &er, sizeof(er), NULL)) == EOK) {
        printf("error: fault 0x%08x rec_tec 0x%08x err_cnt 0x%08x status 0x%08x\n", er.drvr1, er.drvr2,
               er.drvr3, er.drvr4);
    }
    return EOK;
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "usage: canfd-test send DEV ID LEN [HEXDATA] | recv DEV [COUNT] | info DEV\n");
        return 1;
    }
    int fd = open(argv[2], O_RDWR);
    if (fd == -1) {
        fprintf(stderr, "open %s: %s\n", argv[2], strerror(errno));
        return 1;
    }
    int rc;
    if (strcmp(argv[1], "send") == 0) {
        rc = do_send(fd, argc, argv);
    } else if (strcmp(argv[1], "recv") == 0) {
        rc = do_recv(fd, argv[2], argc > 3 ? atoi(argv[3]) : 0);
    } else if (strcmp(argv[1], "info") == 0) {
        rc = do_info(fd);
    } else if (strcmp(argv[1], "pulse") == 0 && argc > 3) {
        /* an fd is a connection to the driver's channel, so it carries pulses */
        rc = MsgSendPulse(fd, -1, atoi(argv[3]), 0) == -1 ? errno : EOK;
        printf("MsgSendPulse code %d: %s\n", atoi(argv[3]), strerror(rc));
    } else {
        rc = EINVAL;
    }
    close(fd);
    return rc == EOK ? 0 : 1;
}
