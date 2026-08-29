// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The saved frame is a trap_frame (kickos/arch/trap.h) whether an interrupt built it or
// arch_context_init did; every resume is an iretq.

#ifndef KICKOS_ARCH_CONTEXT_H
#define KICKOS_ARCH_CONTEXT_H

#include <stdint.h>

struct arch_context
{
    // Saved stack pointer: the base (lowest address) of the thread's current save frame.
    uintptr_t sp;

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    uint32_t trace_tid;
#endif

    uintptr_t stack_lo;
    uintptr_t stack_hi;

    // TOP of this thread's kernel stack, seated by thread_create before arch_context_init
    // and preserved across arch_ctx_redirect. Zero for a TCB outside the pool. Every switch
    // publishes it into TSS.rsp0 and the per-core block.
    uintptr_t kernel_sp;
};

#endif
