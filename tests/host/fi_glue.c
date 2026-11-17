/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * src/ctucanfd_fi.c on the fake core. The injector reads and writes the
 * hardware through ctucan_raw_rd() and ctucan_raw_wr(), plain memory
 * accesses on a mapped core; here they go to the fake core's accessors
 * (fake_core_fi.c), so the fake's FIFO and TX states sit under the overlay.
 */
#include "ctucanfd_hw.h"

uint32_t fake_core_rd(const ctucan_hw_t *hw, uint32_t off);
void     fake_core_wr(const ctucan_hw_t *hw, uint32_t off, uint32_t val);

#define ctucan_raw_rd fake_core_rd
#define ctucan_raw_wr fake_core_wr
#include "ctucanfd_fi.c"
