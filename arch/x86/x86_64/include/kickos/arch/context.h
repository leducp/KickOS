// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The saved frame is a trap_frame (kickos/arch/trap.h) whether an interrupt built it or
// arch_context_init did; every resume is an iretq.

#ifndef KICKOS_ARCH_CONTEXT_H
#define KICKOS_ARCH_CONTEXT_H

#include <stdint.h>

// This arch has I/O ports, which a spawn's port windows grant one range at a time, at most
// KICKOS_ARCH_PORT_RANGES of them, the ceiling of KICKOS_MAX_THREAD_WINDOWS (thread.cc).
#define KICKOS_ARCH_HAS_PORTS 1
#define KICKOS_ARCH_PORT_RANGES 4

// One port window: ports base through last, both included.
struct arch_port_range
{
    uint16_t base;
    uint16_t last;
};

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

    // The port windows this thread holds, seated at spawn and dropped at its exit, and the
    // whole of its port possession record. A switch opens them in the core's I/O permission
    // bitmap when they differ from the set loaded there.
    struct arch_port_range ports[KICKOS_ARCH_PORT_RANGES];
    uint8_t port_count;
};

#endif
