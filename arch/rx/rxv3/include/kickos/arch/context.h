// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// rxv3: struct arch_context. Every switch is deferred through the SWINT handler
// (switch.S), which saves the interrupted registers on the thread's own stack and the PC and
// PSW here, in kernel memory: the PSW carries PM, so no copy of it may sit where the thread or
// a sibling can write.
//
// Every thread runs on its own USP (PSW.U=1), supervisor (PM=0) for a kernel thread and user
// (PM=1) for a user one. The ISP is reserved for exception entry and boot.

#ifndef KICKOS_ARCH_CONTEXT_H
#define KICKOS_ARCH_CONTEXT_H

// arch_switch pends the switcher exception, which installs the incoming context.
#define KICKOS_ARCH_SWITCH_LANDS_LATER 1

// RX has no thread pointer and no native TLS: the emutls override owns the block layout.
#define KICKOS_ARCH_TLS_TCB 0

#include <stdint.h>

struct arch_context
{
    // Saved stack pointer (R0) for this thread: points at the lowest word of the
    // saved frame (A0 low .. R15), on the thread's USP. Written by
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

    // TOP of the kernel stack this thread's privileged dispatch runs on, seated once per pool
    // thread by thread_create. The syscall trap points the USP onto it before svc_trampoline
    // runs, and kickos_rx_pendsw's USP guard accepts a USP inside the block below it as well as
    // one inside the user stack, because a thread preempted mid-dispatch is running there. Read
    // at F_CTX_KERNEL_SP in switch.S. Zero for a TCB outside the pool, which reaches neither
    // site.
    uintptr_t kernel_sp;

    // The PC and PSW the saved frame resumes with, which _kickos_rx_restore stages on the ISP
    // for its RTE. Written by every switch-out and by arch_context_init.
    uint32_t resume_pc;
    uint32_t resume_psw;
};

#endif
