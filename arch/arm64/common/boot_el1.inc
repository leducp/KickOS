/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The arm64 boot entry and tail every chip runs. A chip's _start opens with
 * KICKOS_ARM64_BOOT_ENTRY, handles its entry level from x0, branching to .Lbad_el for a level it
 * refuses, and invokes KICKOS_ARM64_BOOT_EL1 once it is at EL1 with SP on the physical alias of
 * _estack and x9 = __kickos_arm64_va_base: the translation regime, the MMU, the jump to the high
 * half and the C runtime. The chip defines kickos_arm64_bad_el.
 */

#ifndef KICKOS_ARCH_ARM64_COMMON_BOOT_EL1_INC
#define KICKOS_ARCH_ARM64_COMMON_BOOT_EL1_INC

#include <vmsa.h>

.macro KICKOS_ARM64_BOOT_ENTRY
    ldr     x9, =__kickos_arm64_va_base

    /* Before the first stack write: with SPSel 0 the `mov sp` below loads SP_EL0 and leaves
       SP_ELx uninitialised. The vector table's first four slots are unreachable only while
       SPSel is 1. */
    msr     SPSel, #1
    isb

    /* SP before the EL check, so the refusal path can call C. */
    ldr     x0, =_estack            /* boot/exception stack: top of RAM, 16-byte aligned */
    sub     x0, x0, x9
    mov     sp, x0

    mrs     x0, CurrentEL           /* CurrentEL.EL is bits [3:2]; EL1 == 0b01 */
    lsr     x0, x0, #2
    and     x0, x0, #3
.endm

.macro KICKOS_ARM64_BOOT_EL1
    /* CPACR_EL1.FPEN resets to TRAPPING, so the C runtime's q-register copies would trap. */
    mov     x0, #(3 << 20)          /* FPEN=0b11: no trapping at either level */
    msr     CPACR_EL1, x0
    isb

    ldr     x0, =MAIR_VALUE
    msr     MAIR_EL1, x0
    ldr     x0, =TCR_VALUE
    msr     TCR_EL1, x0
    /* Both roots are identity-linked, so these literals are already physical. */
    ldr     x0, =kickos_arm64_ident_table
    msr     TTBR0_EL1, x0
    ldr     x0, =kickos_arm64_kernel_table
    msr     TTBR1_EL1, x0

    /* The tables have to be visible to the walker before the walker exists. */
    dsb     sy
    isb
    tlbi    vmalle1
    dsb     sy
    isb

    /* Before the MMU: an exception with no vector base runs whatever bytes VBAR points at.
       The reporter it reaches touches only the console and .rodata, both live here. Reseated
       at its high address in kickos_arm64_boot_high. */
    ldr     x0, =kickos_armv8a_vectors
    sub     x0, x0, x9
    msr     VBAR_EL1, x0
    isb

    mrs     x0, SCTLR_EL1
    orr     x0, x0, #SCTLR_M
    orr     x0, x0, #SCTLR_C
    orr     x0, x0, #SCTLR_I
    msr     SCTLR_EL1, x0
    isb                             /* the next instruction is fetched translated */

    /* TTBR0's identity map is what keeps this section fetching across the write above.
       Through a register: the displacement to the high half exceeds every branch's. */
    ldr     x0, =kickos_arm64_boot_high
    br      x0

    /* The level stays in x0 as the argument. A direct b, PC-relative: a literal would be the
       high VA, and the MMU is still off. */
.Lbad_el:
    b       kickos_arm64_bad_el     /* noreturn, the chip's own */

/* Reached through TTBR1; SP and VBAR_EL1 still hold the physical aliases. */
    .section .text.boot_high, "ax"
    .cfi_sections .debug_frame      /* an .eh_frame cannot reach the high half */
    .global kickos_arm64_boot_high
kickos_arm64_boot_high:
    .cfi_startproc
    .cfi_undefined x30
    ldr     x0, =_estack
    mov     sp, x0
    ldr     x0, =kickos_armv8a_vectors
    msr     VBAR_EL1, x0
    isb

    mov     x29, xzr
    bl      Reset_Handler           /* C runtime + kmain (noreturn) */
.Lhang:
    wfi
    b       .Lhang
    .cfi_endproc
.endm

#endif
