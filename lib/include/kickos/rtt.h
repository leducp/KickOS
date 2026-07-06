// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// SEGGER RTT up-channel console output. Bytes written here land in a control
// block (symbol _SEGGER_RTT) that J-Link scans target RAM for; a host
// JLinkRTTClient drains them over the debug channel with no UART. Clean-room:
// only the documented control-block memory layout is reproduced; the ring
// logic is original.

#ifndef KICKOS_RTT_H
#define KICKOS_RTT_H

#include <stddef.h>
#include <stdint.h>

// Publish barrier: ordering between the ring-buffer payload write and the WrOff
// update the host reads. Compiler-only by default -- correct on the in-order,
// single-core M-class parts KickOS targets today (a store-store on the same core
// needs no hardware fence). A weakly-ordered core (Xtensa / RISC-V, or a future
// multi-core part) must override this with a real fence via a CMake-injected
// compile definition (-DKICKOS_RTT_PUBLISH_BARRIER=...); lib/ is a leaf, so it
// must NOT reach up into an arch header for the barrier -- the build injects it.
#ifndef KICKOS_RTT_PUBLISH_BARRIER
#define KICKOS_RTT_PUBLISH_BARRIER() __asm volatile("" ::: "memory")
#endif

#ifdef __cplusplus
extern "C"
{
#endif

// Channel 0: human-readable console text (byte stream, tail-droppable).
void kickos_rtt_write(char const* buf, size_t n);

// Channel 1: binary telemetry, RECORD-ATOMIC. Writes the whole record or none
// (a torn binary record desynchronizes the decoder permanently). Returns 1 if
// written, 0 if the ring could not fit it (caller counts the drop). NoBlockSkip.
int kickos_rtt_write_record_ch1(uint8_t const* rec, size_t n);

// Drain up to `max` bytes of pending ch1 data (rd_off -> wr_off), advancing
// rd_off; returns the number copied (0 when empty). Host-side use: the sim
// backend flushes the ring to a file at shutdown for the offline decoder.
size_t kickos_rtt_ch1_drain(char* out, size_t max);

#ifdef __cplusplus
}
#endif

#endif
