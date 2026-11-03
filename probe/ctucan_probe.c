/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * ctucan-probe: find the CTU CAN FD PCI board (as emulated by QEMU's
 * ctucan_pci), attach through the QNX PCI server, map both BARs and print
 * the identification registers.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <pci/pci.h>

#define CTUCAN_PCI_VID      0x1760u /* TEDIA */
#define CTUCAN_PCI_DID      0xff00u /* CTU CAN FD, PCI bridge v2.1 */

#define BRIDGE_ID           0x0000u /* BAR0: [31:28]=0xC magic, [3:0]=core count */
#define CORE_STRIDE         0x4000u /* BAR1: one core every 16 KiB */

/* CTU CAN FD core register offsets (datasheet "Memory map") */
#define REG_DEVICE_ID       0x00u   /* [15:0] DEVICE_ID, [23:16] minor, [31:24] major */
#define REG_MODE            0x04u
#define REG_STATUS          0x08u
#define REG_FILTER_CONTROL  0x5cu   /* [15:0] FILTER_CONTROL, [31:16] FILTER_STATUS */
#define REG_RX_MEM_INFO     0x60u
#define REG_TX_STATUS       0x70u
#define REG_YOLO            0x90u

#define CTUCAN_DEVICE_ID    0xcafdu

static uint32_t rd32(volatile uint8_t *base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Without I/O privileges the probe died with SIGSEGV during PCI access.
     * ThreadCtl() (QNX 8.0 library reference): level 1 grants port I/O and
     * interrupt disable/enable to the calling thread. */
    if (ThreadCtl(_NTO_TCTL_IO_LEVEL, (void *)(uintptr_t)_NTO_IO_LEVEL_1) == -1) {
        fprintf(stderr, "ThreadCtl(_NTO_TCTL_IO_LEVEL): %s\n", strerror(errno));
        return 1;
    }

    pci_err_t err;
    pci_bdf_t bdf = pci_device_find(0, CTUCAN_PCI_VID, CTUCAN_PCI_DID, PCI_CCODE_ANY);

    if (bdf == PCI_BDF_NONE) {
        fprintf(stderr, "no %04x:%04x device found\n", CTUCAN_PCI_VID, CTUCAN_PCI_DID);
        return 1;
    }
    printf("found %04x:%04x at B%u:D%u:F%u\n", CTUCAN_PCI_VID, CTUCAN_PCI_DID,
           PCI_BUS(bdf), PCI_DEV(bdf), PCI_FUNC(bdf));

    pci_devhdl_t hdl = pci_device_attach(bdf, pci_attachFlags_EXCLUSIVE_OWNER, &err);
    if (hdl == NULL) {
        fprintf(stderr, "pci_device_attach: %s\n", pci_strerror(err));
        return 1;
    }

    pci_ba_t ba[7];
    int_t nba = 7;
    err = pci_device_read_ba(hdl, &nba, ba, pci_reqType_e_UNSPECIFIED);
    if (err != PCI_ERR_OK || nba < 2) {
        fprintf(stderr, "pci_device_read_ba: %s (nba=%d)\n", pci_strerror(err), (int)nba);
        return 1;
    }

    pci_irq_t irq[4];
    int_t nirq = 4;
    err = pci_device_read_irq(hdl, &nirq, irq);
    if (err == PCI_ERR_OK && nirq > 0) {
        printf("irq: %d (of %d)\n", (int)irq[0], (int)nirq);
    } else {
        printf("irq: none (%s)\n", pci_strerror(err));
    }

    volatile uint8_t *bar[2] = { NULL, NULL };
    for (int i = 0; i < nba; i++) {
        printf("BAR%d: %s addr=0x%" PRIx64 " size=0x%" PRIx64 "\n", (int)ba[i].bar_num,
               ba[i].type == pci_asType_e_MEM ? "MEM" : "IO", (uint64_t)ba[i].addr, ba[i].size);
        if (ba[i].bar_num > 1 || ba[i].type != pci_asType_e_MEM) {
            continue;
        }
        void *p = mmap_device_memory(NULL, ba[i].size, PROT_READ | PROT_WRITE | PROT_NOCACHE,
                                     0, ba[i].addr);
        if (p == MAP_FAILED) {
            fprintf(stderr, "mmap_device_memory BAR%d: %s\n", (int)ba[i].bar_num, strerror(errno));
            return 1;
        }
        bar[ba[i].bar_num] = p;
    }
    if (bar[0] == NULL || bar[1] == NULL) {
        fprintf(stderr, "BAR0/BAR1 not mapped\n");
        return 1;
    }

    uint32_t bid = rd32(bar[0], BRIDGE_ID);
    unsigned ncores = bid & 0xfu;
    printf("BAR0+0x0000 bridge id: 0x%08x (cores=%u)\n", bid, ncores);

    for (unsigned c = 0; c < ncores && c < 4; c++) {
        volatile uint8_t *core = bar[1] + c * CORE_STRIDE;
        uint32_t idv = rd32(core, REG_DEVICE_ID);
        printf("core%u DEVICE_ID=0x%04x VERSION=%u.%u (raw 0x%08x)%s\n", c, idv & 0xffffu,
               (idv >> 24) & 0xffu, (idv >> 16) & 0xffu, idv,
               (idv & 0xffffu) == CTUCAN_DEVICE_ID ? " OK" : " UNEXPECTED");
        printf("core%u MODE=0x%08x STATUS=0x%08x RX_MEM_INFO=0x%08x TX_STATUS=0x%08x YOLO=0x%08x\n",
               c, rd32(core, REG_MODE), rd32(core, REG_STATUS), rd32(core, REG_RX_MEM_INFO),
               rd32(core, REG_TX_STATUS), rd32(core, REG_YOLO));
        /* FILTER_STATUS bits SFA, SFB, SFC, SFR say which acceptance filters were synthesised */
        uint32_t fs = rd32(core, REG_FILTER_CONTROL) >> 16;
        printf("core%u FILTER_STATUS=0x%04x filters present:%s%s%s%s%s\n", c, fs, fs & 1u ? " A" : "",
               fs & 2u ? " B" : "", fs & 4u ? " C" : "", fs & 8u ? " range" : "", fs & 0xfu ? "" : " none");
    }

    pci_device_detach(hdl);
    return 0;
}
