// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// rxv3: struct arch_context for the Renesas RX72M (RXv3 core). Every switch is
// deferred through the SWINT handler (switch.S), which saves the FULL interrupted
// register set -- R1-R15, FPSW, the two accumulators, and the INT-stacked PC/PSW
// -- on the thread's own stack, exactly like the ARM PendSV backend. So the only
// per-thread state the kernel holds is the saved stack pointer; PC/PSW/GPRs all
// live in the frame that `sp` points at, not in this struct.
//
// Every KickOS thread runs on its own stack selected by PSW.U=1 (the USP), in
// supervisor (PM=0) for a kernel thread or user (PM=1) for a user thread. The
// interrupt stack (ISP, PSW.U=0) is reserved for exception entry and boot, so a
// thread blocked mid-syscall keeps its continuation on its own per-thread USP.

#ifndef KICKOS_ARCH_CONTEXT_H
#define KICKOS_ARCH_CONTEXT_H

#include <stdint.h>

struct arch_context
{
    // Saved stack pointer (R0) for this thread: points at the lowest word of the
    // saved full-context frame (A0 low .. PSW), on the thread's USP. Written by
    // the SWINT switcher on switch-out, read on switch-in / by arch_start. FIELD 0
    // -- switch.S hard-codes this offset (see the static_assert in arch_rxv3.cc).
    uint32_t sp;

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // Telemetry only: the owning thread's trace id, stamped once by
    // arch_trace_stamp_id (thread_create) and read by the SWINT switcher to emit
    // the {from,to} SWITCH record from the physically-swapped contexts. OFFSET 4
    // -- switch.S hard-codes it (the `4[r15]` reads). Elided with the whole
    // telemetry path when telemetry is compiled out.
    uint32_t trace_tid;
#endif
};

#endif
