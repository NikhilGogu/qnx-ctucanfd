/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The fake core for the fault-injection build: in that build the injector
 * (src/ctucanfd_fi.c) owns ctucan_rd() and ctucan_wr(), so the fake's
 * accessors are renamed here and the injector reaches the core through them
 * (fi_glue.c).
 */
#define ctucan_rd fake_core_rd
#define ctucan_wr fake_core_wr
#include "fake_core.c"
