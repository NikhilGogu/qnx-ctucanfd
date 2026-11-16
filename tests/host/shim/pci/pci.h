/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests only. A stand-in for the PCI server names from QNX's
 * <pci/pci.h> that src/dev-can-ctucanfd.c uses. It is written from how the
 * driver uses them, not copied from the QNX header; types, values and
 * layouts are not QNX's. tests/host/fake_libcan.c implements one board with
 * one core. Never put this directory on the include path of a QNX build.
 */
#ifndef CTUCAN_HOST_SHIM_PCI_H
#define CTUCAN_HOST_SHIM_PCI_H

#include <stdint.h>

typedef int      int_t;
typedef uint32_t pci_bdf_t;
typedef int      pci_irq_t;
typedef int      pci_err_t;
typedef struct pci_dev *pci_devhdl_t;

typedef enum { pci_asType_e_NONE, pci_asType_e_IO, pci_asType_e_MEM } pci_asType_e;
typedef enum { pci_reqType_e_UNSPECIFIED } pci_reqType_e;
typedef enum { pci_attachFlags_EXCLUSIVE_OWNER = 1 } pci_attachFlags_e;

typedef struct {
    uint64_t     addr, size;
    pci_asType_e type;
    int          bar_num;
} pci_ba_t;

#define PCI_ERR_OK      0
#define PCI_BDF_NONE    0xffffffffu
#define PCI_CCODE_ANY   0xffffffffu
#define PCI_BUS(bdf)    (((bdf) >> 8) & 0xffu)
#define PCI_DEV(bdf)    (((bdf) >> 3) & 0x1fu)
#define PCI_FUNC(bdf)   ((bdf) & 0x7u)

pci_bdf_t    pci_device_find(unsigned idx, unsigned vid, unsigned did, unsigned ccode);
pci_devhdl_t pci_device_attach(pci_bdf_t bdf, pci_attachFlags_e flags, pci_err_t *err);
pci_err_t    pci_device_detach(pci_devhdl_t hdl);
pci_err_t    pci_device_read_ba(pci_devhdl_t hdl, int_t *nba, pci_ba_t *ba, pci_reqType_e req);
pci_err_t    pci_device_read_irq(pci_devhdl_t hdl, int_t *nirq, pci_irq_t *irq);
const char  *pci_strerror(pci_err_t err);

#endif
