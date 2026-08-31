/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The AArch64 secondary-core entry. The chip layer owns the release itself (PSCI CPU_ON on
 * this family) and hands it kickos_armv8a_secondary_entry.
 *
 * A RELEASED CORE ARRIVES AT EL1 WITH THE MMU OFF AND NOTHING SEATED, so this entry runs
 * untranslated and links into .text.init, the identity-linked span where the chip's `_start`
 * also lives: VMA == LMA there, so the value of this symbol IS the address the release
 * mechanism takes. What the entry reads before translation is on is read at a PHYSICAL alias,
 * which is what __kickos_arm64_va_base is subtracted for, and the record it reads was cleaned
 * to the point of coherency by the core that wrote it (arch_armv8a.cc): an MMU-off access is
 * non-cacheable whatever SCTLR_EL1 says.
 */

#include <kickos/board_config.h>

#if KICKOS_NUM_CORES > 1

/* Displacements into struct armv8a_boot_regs, and the stride of the per-CPU block array.
   static_asserted against sizeof and offsetof in arch_armv8a.cc. */
#define BOOT_MAIR       0
#define BOOT_TCR        8
#define BOOT_TTBR0      16
#define BOOT_TTBR1      24
#define BOOT_SCTLR      32
#define BOOT_CPACR      40
#define PERCPU_SIZE     24

    .section .text.init, "ax"
    .global kickos_armv8a_secondary_entry
kickos_armv8a_secondary_entry:
    msr     daifset, #0xF           /* the release mechanism promises no PSTATE */
    msr     SPSel, #1               /* SP_ELx: the level the vector table's shape assumes */
    isb

    /* IDENTITY BEFORE MEMORY, x19 carrying it to the far side of the MMU. A core whose Aff0
       names no block has nothing to index, so it stops here rather than seat a wild
       pointer. The index must land in [1, KICKOS_NUM_CORES): zero is the boot core's, and a
       released core reaching it would seat the boot core's block, take its stack and publish
       its arrival. Aff0 restarts per cluster, so a multi-cluster part reaches zero here. */
    mrs     x19, mpidr_el1
    and     x19, x19, #0xFF                 /* MPIDR_EL1.Aff0 (arch_armv8a.cc) */
    cmp     x19, #KICKOS_NUM_CORES
    b.hs    .Lsecondary_stop
    cbz     x19, .Lsecondary_stop

    ldr     x9, =__kickos_arm64_va_base

    /* TPIDR_EL1 AHEAD OF THE MMU, holding the block's HIGH address: seated after translation
       came on there would be a window in which an exception reads it unseated and builds a
       privileged frame on whatever reset left in it. */
    ldr     x1, =kickos_armv8a_percpu
    mov     x2, #PERCPU_SIZE
    madd    x1, x19, x2, x1
    msr     tpidr_el1, x1

    ldr     x10, =kickos_armv8a_boot_regs
    sub     x10, x10, x9                    /* the record's physical alias */
    ldp     x2, x3, [x10, #BOOT_MAIR]
    msr     mair_el1, x2
    msr     tcr_el1, x3
    ldp     x4, x5, [x10, #BOOT_TTBR0]
    msr     ttbr0_el1, x4
    msr     ttbr1_el1, x5
    ldr     x6, [x10, #BOOT_CPACR]
    msr     cpacr_el1, x6                   /* FPEN: a q-register copy traps without it */
    ldr     x7, [x10, #BOOT_SCTLR]

    /* The tables have to be visible to the walker before the walker exists, and this core
       carries whatever its own reset left in its caches and its TLB. */
    dsb     sy
    isb
    tlbi    vmalle1
    ic      iallu
    dsb     sy
    isb

    /* At its physical alias, for the window still being fetched untranslated. Reseated at its
       high address past the branch below, as the chip's `_start` does for the primary. */
    ldr     x0, =kickos_armv8a_vectors
    sub     x0, x0, x9
    msr     vbar_el1, x0
    isb

    msr     sctlr_el1, x7                   /* M, C and I as the primary runs with them */
    isb                                     /* the next instruction is fetched translated */

    /* Through a register: the displacement to the high half exceeds every branch's. */
    ldr     x0, =kickos_armv8a_secondary_high
    br      x0

/* A core with no block to stand on: the record that would let it report is the one it could
   not index. */
.Lsecondary_stop:
    wfi
    b       .Lsecondary_stop
    .ltorg

/* Reached through TTBR1, x19 still carrying this core's index. SP is unset and VBAR_EL1 holds
 * the physical alias the untranslated half needed.
 */
    .section .text.secondary_high, "ax"
    .global kickos_armv8a_secondary_high
kickos_armv8a_secondary_high:
    adrp    x0, kickos_armv8a_boot_sp
    add     x0, x0, #:lo12:kickos_armv8a_boot_sp
    ldr     x1, [x0, x19, lsl #3]
    mov     sp, x1

    ldr     x0, =kickos_armv8a_vectors
    msr     vbar_el1, x0
    isb

    mov     x0, x19
    bl      kickos_armv8a_park_core         /* noreturn (arch_armv8a.cc) */
.Lsecondary_high_stop:
    wfi
    b       .Lsecondary_high_stop

#endif

    .end
