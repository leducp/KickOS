// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// AArch64: register state lives in a flat save frame on the thread's own stack, so a
// thread is described by one pointer, the base of that frame (switch.S).

#ifndef KICKOS_ARCH_CONTEXT_H
#define KICKOS_ARCH_CONTEXT_H

// Variant-1 TLS: tpidr_el0 points at a 16-byte reserved TCB and the first thread_local
// sits at tp + 16, so the carve owes these bytes on top of .tdata + .tbss.
#define KICKOS_ARCH_TLS_TCB 16

#include <stdint.h>

struct arch_context
{
    // Saved stack pointer: the base (lowest address) of the thread's current save frame.
    uintptr_t sp;

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // Telemetry only: the owning thread's trace id, stamped once by
    // arch_trace_stamp_id (thread_create) and read by the switch path to emit the
    // {from,to} SWITCH record from the physically-swapped contexts.
    uint32_t trace_tid;
#endif

    // Bookkeeping only here: no trap entry validates an sp against them, a trap from EL0
    // replacing SP_EL0 with SP_EL1 rather than storing through the sp the thread chose.
    // Read by arch_ctx_redirect and by the frame guards in kernel/init/fault.cc.
    uintptr_t stack_lo;
    uintptr_t stack_hi;

#if defined(KICKOS_TLS) && KICKOS_TLS
    // TPIDR_EL0 while this thread runs. SEATED rather than masked out of the stack pointer,
    // which is what frees this arch of the power-of-two stride the mask imposes (F7).
    // Write-once per thread, so no switch saves it.
    uintptr_t tls_base;
#endif

    // TOP of this thread's kernel stack, seated by thread_create BEFORE arch_context_init,
    // which reads it to place an unprivileged thread's first frame, and preserved across
    // arch_ctx_redirect. Write-once per slot, so no switch saves it. Zero for a TCB outside
    // the pool, which is privileged and never returns to EL0.
    uintptr_t kernel_sp;
};

#endif
