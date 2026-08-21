// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// rxv3: struct arch_context for the Renesas RX72M (RXv3 core). Every switch is
// deferred through the SWINT handler (switch.S), which saves the FULL interrupted
// register set (R1-R15, FPSW, the two accumulators, and the INT-stacked PC/PSW) on
// the thread's own stack. So the only per-thread state the kernel holds is the saved
// stack pointer: PC/PSW/GPRs all live in the frame that `sp` points at.
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
    // the SWINT switcher on switch-out, read on switch-in / by arch_start. MUST STAY
    // FIELD 0: switch.S hard-codes the offset (static_assert in arch_rxv3.cc).
    uint32_t sp;

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // Telemetry only: the owning thread's trace id, stamped once by
    // arch_trace_stamp_id (thread_create) and read by the SWINT switcher to emit
    // the {from,to} SWITCH record from the physically-swapped contexts. MUST STAY AT
    // OFFSET 4: switch.S hard-codes it (the `4[r15]` reads).
    uint32_t trace_tid;
#endif

    // Stack bounds the syscall trap and the SWINT switcher (switch.S) check the live USP
    // against before they build a frame on it (rx_trap_stack.h). Set once by
    // arch_context_init; read at F_CTX_STACK_LO / F_CTX_STACK_HI in switch.S. A USP
    // outside [stack_lo, stack_hi] routes the trap to a panic.
    uint32_t stack_lo;
    uint32_t stack_hi;

    // The kernel stack this thread's privileged dispatch runs on, saved across a switch the
    // way `sp` above saves the USER one. `sp` is and stays the user stack pointer on every
    // backend, so no second field names it; these two are the pair a trusted entry swaps
    // between. Unused until the trap entry transfers to it: the field exists here first so
    // the allocation and the geometry can be reviewed apart from the execution change.
    uint32_t kernel_sp;
};

#endif
