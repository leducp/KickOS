// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The saved frame is a trap_frame (kickos/arch/trap.h) whether an interrupt built it or
// arch_context_init did; every resume is an iretq.
//
// Read by switch.S as well, for the vector-state figures below.

#ifndef KICKOS_ARCH_CONTEXT_H
#define KICKOS_ARCH_CONTEXT_H

// The state components every core enables and every switch saves: x87, SSE and AVX, and
// nothing else even where the processor has more.
#define KICKOS_X86_64_XCR0 7
// XSAVE's standard-format area for that XCR0: the 512-byte legacy region, the 64-byte header
// and the 256 bytes of upper YMM halves.
#define KICKOS_X86_64_XSAVE_SIZE 832
// Every SIMD exception masked, round to nearest: what a new thread's area seeds.
#define KICKOS_X86_64_MXCSR_INIT 0x1F80
// The area's offset in struct arch_context, which switch.S spells and arch_x86_64.cc asserts.
#define KICKOS_X86_64_CTX_XSAVE 64

// IA32_FS_BASE, which every switch-in loads with the incoming context's fs_base, and the
// canonical kernel-half address each core seeds it with once its descriptor tables are loaded,
// which no thread's cell has, so a ring 3 load through a base nobody wrote faults.
#define KICKOS_X86_64_MSR_FS_BASE 0xC0000100u
#define KICKOS_X86_64_FS_POISON 0xFFFF8000F5000000ull

#ifndef __ASSEMBLER__

#include <stdint.h>

// This arch has I/O ports, which a spawn's port windows grant one range at a time, at most
// KICKOS_ARCH_PORT_RANGES of them, the ceiling of KICKOS_MAX_THREAD_WINDOWS (thread.cc).
#define KICKOS_ARCH_HAS_PORTS 1
#define KICKOS_ARCH_PORT_RANGES 4

// The vector state area makes the TCB too large to pin to the byte here, so
// kernel/include/kickos/thread.h holds sizeof(Thread) to this ceiling instead.
#define KICKOS_ARCH_THREAD_SIZE_CEILING 2048

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

    uintptr_t stack_lo;
    uintptr_t stack_hi;

    // TOP of this thread's kernel stack, seated by thread_create before arch_context_init
    // and preserved across arch_ctx_redirect. Zero for a TCB outside the pool. Every switch
    // publishes it into TSS.rsp0 and the per-core block.
    uintptr_t kernel_sp;

    // The user address of the cell at the top of an unprivileged thread's stack whose first
    // word is its struct _reent pointer, which libc's __getreent reads at %fs:0; 0 for a
    // privileged context, which has no cell. Every switch-in loads it into IA32_FS_BASE.
    uintptr_t fs_base;

    // The port windows this thread holds, seated at spawn and dropped at its exit, and the
    // whole of its port possession record. A switch opens them in the core's I/O permission
    // bitmap when they differ from the set loaded there.
    struct arch_port_range ports[KICKOS_ARCH_PORT_RANGES];
    uint8_t port_count;
    // Bits 2r, 2r + 1: the place of ports[r] in the thread's spawn list.
    uint8_t port_places;

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    uint32_t trace_tid;
#endif

    // The thread's x87, SSE and AVX state while it is switched out, in XSAVE's standard
    // format for KICKOS_X86_64_XCR0. Saved and restored on both switch paths and restored at
    // a core's first thread.
    uint8_t xsave[KICKOS_X86_64_XSAVE_SIZE] __attribute__((aligned(64)));
};

#endif

#endif
