// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RISC-V RV32IMAC: the state the switcher needs to resume a thread. Every register lives in
// one flat save frame on the thread's stack (switch.S), the same format for a voluntary block
// and a preemptive wake, so a thread is described by the base of that frame. Its privilege is
// the frame's mstatus.MPP.
//
// switch.S reads every field at a literal displacement (rv_trap_stack.h, F_CTX_TLS_BASE);
// arch_rv32imac.cc static_asserts them.

#ifndef KICKOS_ARCH_CONTEXT_H
#define KICKOS_ARCH_CONTEXT_H

// RISC-V TLS is variant 2: tp IS the block start and the first thread_local sits AT it, so
// the ABI reserves nothing below the thread pointer.
#define KICKOS_ARCH_TLS_TCB 0

#include <stdint.h>

struct arch_context
{
    // The base of the thread's current save frame.
    uint32_t sp;

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // Stamped once by arch_trace_stamp_id; the switch path reads it to emit the SWITCH record.
    uint32_t trace_tid;
#endif

    // The bounds trap_entry checks an interrupted U-mode sp against before it stores a frame
    // through it: a U-mode thread can aim its sp at kernel memory, which the software M-mode
    // prologue would then write.
    uint32_t stack_lo;
    uint32_t stack_hi;

    // TOP of the kernel stack this thread's privileged dispatch runs on, seated by
    // thread_create and preserved across arch_ctx_redirect; write-once per slot, so no switch
    // saves it. Zero for a TCB outside the pool.
    uintptr_t kernel_sp;

#if defined(KICKOS_TLS) && KICKOS_TLS
    // tp while this thread runs, written at every resume and never read back from the
    // register, which U-mode can write. Write-once per thread.
    uint32_t tls_base;
#endif
};

#endif
