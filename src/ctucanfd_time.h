/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Timestamp arithmetic of dev-can-ctucanfd, kept apart from any QNX header so
 * the host unit tests (tests/host/test_time.c) can check it.
 *
 * Frames and CAN_DEVCTL_GET_TIMESTAMP carry 32-bit microseconds, which wrap
 * every 2^32 us. The driver keeps one 32-bit offset that
 * CAN_DEVCTL_SET_TIMESTAMP chooses so the 32-bit value reads what the client
 * asked for at that moment. All arithmetic is unsigned, so the wrap is
 * defined behaviour. The 64-bit value (CTUCAN_DEVCTL_GET_TIMESTAMP64) is the
 * monotonic time plus the same offset: its low 32 bits always equal the
 * 32-bit timestamp, and it does not wrap.
 */
#ifndef CTUCANFD_TIME_H
#define CTUCANFD_TIME_H

#include <stdint.h>

/* 32-bit timestamp at monotonic time mono_us. */
static inline uint32_t ctucan_ts32(uint64_t mono_us, uint32_t offset)
{
    return (uint32_t)mono_us + offset;
}

/* The offset that makes ctucan_ts32(mono_us, offset) read `wanted`. */
static inline uint32_t ctucan_ts_offset_for(uint64_t mono_us, uint32_t wanted)
{
    return wanted - (uint32_t)mono_us;
}

/* 64-bit timestamp: no wrap, low 32 bits equal to ctucan_ts32(). */
static inline uint64_t ctucan_ts64(uint64_t mono_us, uint32_t offset)
{
    return mono_us + offset;
}

#endif
