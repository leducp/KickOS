/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Read by BOTH switch.S and the C++ side, so each number has one definition.
 *
 * SYSCALL loads no stack pointer, so the entry loads one by hand out of the gs base; the
 * task-state segment's rsp0 serves the interrupt-gate path. Both are written on every switch.
 *
 * DO NOT rename this to a *_trap_stack.h: tests/static/check_trap_redzone_decls.sh uses that
 * suffix to pick the arches it holds to a full declaration, and nothing here has been measured
 * under -fcallgraph-info.
 */

#ifndef KICKOS_ARCH_RING3_H
#define KICKOS_ARCH_RING3_H

/* Byte offsets into the per-core block, reached through the gs base. kernel_sp is at 0
 * because that is the one the entry loads before it can address anything else. */
#define KICKOS_X86_64_CPU_KERNEL_SP  0
#define KICKOS_X86_64_CPU_USER_RSP   8
#define KICKOS_X86_64_CPU_SIZE      16

/* The selectors the syscall entry writes into the frame it builds; arch_syscall issues the
 * instruction only from ring 3 (switch.S). */
#define KICKOS_X86_64_SEL_USER_CODE 0x23
#define KICKOS_X86_64_SEL_USER_DATA 0x1b

/* The flags a frame returning to ring 3 may carry, and the ones it must.
 *   keep: carry, parity, adjust, zero, sign, direction, overflow
 *   force: the interrupt flag, and bit 1, which reads as one on every x86 processor
 * Everything else is dropped, the trap flag, the nested-task flag, the alignment-check flag
 * and the I/O privilege level included.
 */
#define KICKOS_X86_64_RFLAGS_USER_KEEP  0x0cd5
#define KICKOS_X86_64_RFLAGS_USER_FORCE 0x0202

/* The vector the syscall entry stamps into the frame it builds. Above the 32 the processor
 * defines and clear of the three the local APIC delivers (apic.h). */
#define KICKOS_X86_64_VECTOR_SYSCALL 0x100

#ifndef __ASSEMBLER__

#include <stdint.h>
#include <stddef.h>

namespace kickos::x86_64
{
    // One per core, reached through the gs base. Written by the arch layer on every switch.
    struct cpu_block
    {
        uint64_t kernel_sp;
        uint64_t user_rsp;
    };

    // Make ring 3 reachable and arm the fast syscall pair. Called from the chip's arch_init
    // AFTER desc_init. The range is the conventional memory user stacks are carved out of.
    void ring3_init(uintptr_t ram_base, size_t ram_size);

    // The running thread's kernel stack top, published to the block above.
    void cpu_set_kernel_sp(uint64_t top);
    uint64_t cpu_kernel_sp(void);

    // Is [ptr, ptr + len) inside ONE section of this image that the loader mapped, and
    // writable where asked?
    bool image_range_mapped(uintptr_t ptr, size_t len, bool need_write);

    uintptr_t image_base(void);
    size_t image_size(void);
    unsigned image_sections(void);

    // What ring3_init found and did, for the boot report. `granted` counts the leaf entries
    // that gained the user bit; `already` counts those that carried it on arrival.
    unsigned user_leaves_granted(void);
    unsigned user_leaves_already(void);

    // The exposure census over the tables this port's own grant walked. `walked` counts the
    // DISTINCT tables, by physical address; `exposed` counts those an unprivileged thread can
    // reach. The grant's unit is a leaf, so a large leaf covering the end of a range exposes
    // every byte to that leaf's end.
    unsigned user_tables_exposed(void);
    unsigned user_tables_walked(void);
    uint64_t control_flags(void);

    // CR0 as ring3_init found it. This firmware sets its write-protect bit and hands over
    // read-only translation tables, so the grant has to lift it.
    uint64_t control_flags0(void);
}

#endif

#endif
