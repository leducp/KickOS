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

// The kernel's own map of RAM is Normal write-back whatever a task's leaf says.
#define KICKOS_ARCH_ALIAS_DCACHE 1
#define KICKOS_ARCH_IRQ_WINDOW 1

#include <stdint.h>

struct arch_context
{
    // Saved stack pointer: the base (lowest address) of the thread's current save frame.
    uintptr_t sp;

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // The owning thread's trace id, stamped once by arch_trace_stamp_id.
    uint32_t trace_tid;
#endif

    // Bookkeeping only: no trap entry validates an sp against them.
    uintptr_t stack_lo;
    uintptr_t stack_hi;

#if KICKOS_TLS
    // TPIDR_EL0 while this thread runs. Write-once per thread, so no switch saves it.
    uintptr_t tls_base;
#endif

    // TOP of this thread's kernel stack, seated by thread_create BEFORE arch_context_init,
    // which reads it to place an unprivileged thread's first frame. Write-once per slot, so no
    // switch saves it. Zero for a TCB outside the pool, which never returns to EL0.
    uintptr_t kernel_sp;
};

#endif
