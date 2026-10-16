/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Read by BOTH switch.S and the C++ side, so each number has one definition.
 *
 * SYSCALL loads no stack pointer, so the entry loads one by hand out of the gs base; the
 * task-state segment's rsp0 serves the interrupt-gate path. Both are written on every switch.
 */

#ifndef KICKOS_ARCH_RING3_H
#define KICKOS_ARCH_RING3_H

/* Byte offsets into the per-core block, reached through the gs base. kernel_sp is at 0
 * because that is the one the entry loads before it can address anything else. */
#define KICKOS_X86_64_CPU_KERNEL_SP  0
#define KICKOS_X86_64_CPU_USER_RSP   8
#define KICKOS_X86_64_CPU_SW_START  16
#define KICKOS_X86_64_CPU_CORE_ID   20
#define KICKOS_X86_64_CPU_SIZE      64

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
    struct alignas(64) cpu_block
    {
        uint64_t kernel_sp;
        uint64_t user_rsp;
        uint32_t sw_start;
        uint32_t core_id;
    };

    // Used only before this processor has installed its GS block. Once ring3_cpu_init
    // completes, arch_cpu_id reads cpu_block::core_id with one GS-relative load.
    uint32_t boot_apic_id(void);

    // CR4.SMEP and CR4.SMAP (Intel SDM Vol 3, section 2.5).
    constexpr uint64_t cr4_guard = (1ull << 20) | (1ull << 21);

    // Whether a core reading back `core_cr4` holds the SMEP and SMAP the boot core's `boot_cr4`
    // does; the other CR4 bits may differ.
    constexpr bool guard_matches(uint64_t boot_cr4, uint64_t core_cr4)
    {
        return ((boot_cr4 ^ core_cr4) & cr4_guard) == 0;
    }

    // Arm the fast syscall pair and the per-core block. Call AFTER desc_init. Opens nothing to
    // ring 3: a task reaches only what its own space maps. Records the boot core's SMEP and SMAP
    // as CR4 reads them back, which arch_uaccess_guard names.
    void ring3_init(void);
    // APs inherit the BSP's shared page tables but need their own GS base and syscall MSRs.
    // Also sets CR4.SMEP and CR4.SMAP where this processor's CPUID offers them.
    void ring3_cpu_init(void);
    // Whether this core's SMEP and SMAP, read back from CR4, are the boot core's. After
    // ring3_init on the boot core and ring3_cpu_init on this one; ap_main refuses the boot on false.
    bool ring3_guard_matches_boot(void);

    // The running thread's kernel stack top, published to the block above.
    void cpu_set_kernel_sp(uint64_t top);
    uint64_t cpu_kernel_sp(void);
}

#endif

#endif
