/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The AArch64 EL1 exception vector table, installed in VBAR_EL1 by boot_el1.inc.
 *
 * 2 KiB aligned, sixteen slots of 0x80 bytes, in four groups of four (synchronous, IRQ, FIQ,
 * SError) by where the exception came from. Every slot is filled: an unfilled one runs whatever
 * bytes follow the table. A slot that is not serviced reports its index and stops.
 *
 * A reporting slot never returns, so it may clobber freely. It passes the slot index and the
 * interrupted x30; everything else the reporter needs is in ESR_EL1, FAR_EL1, ELR_EL1 and
 * SPSR_EL1.
 */

#include <kickos/board_config.h>

    .macro  VEC_SLOT index
    .balign 128
    mov     x0, #\index
    mov     x1, x30                 /* the interrupted LR, before any prologue saves it */
    b       kickos_armv8a_exception
    .endm

/* An IRQ slot resumes, so unlike the reporting slots it preserves everything a callee may
 * clobber plus ELR_EL1 and SPSR_EL1. The body lives in switch.S with the frame it shares with
 * the switcher: one format resumes a preempted thread and one that switched voluntarily.
 */
    .macro  IRQ_SLOT
    .balign 128
    b       kickos_armv8a_irq_entry
    .endm

/* A lower-EL reporting slot seats SP_EL1 from the kernel's own record: reaching one (an AArch32
 * entry or an EL0 SError) means SP_EL1 carries no promise, and the reporter would otherwise
 * build privileged frames on memory an unprivileged thread can write.
 *
 * kernel_sp sits at displacement 0 of the per-core block (percpu.h static_asserts it). Above
 * one core the block address is TPIDR_EL1, seated before the core turns translation on.
 */
    .macro  LOWER_EL_SLOT index
    .balign 128
#if KICKOS_NUM_CORES > 1
    mrs     x0, tpidr_el1
    ldr     x0, [x0]
#else
    adrp    x0, kickos_armv8a_percpu
    ldr     x0, [x0, #:lo12:kickos_armv8a_percpu]
#endif
    mov     sp, x0
    mov     x0, #\index
    mov     x1, x30
    b       kickos_armv8a_exception
    .endm

    .section .text.vectors, "ax"
    .balign 2048
    .global kickos_armv8a_vectors
kickos_armv8a_vectors:
    /* Current EL, SP_EL0: unreachable while SPSel is 1. */
    VEC_SLOT 0                      /* 0x000 synchronous */
    VEC_SLOT 1                      /* 0x080 IRQ         */
    VEC_SLOT 2                      /* 0x100 FIQ         */
    VEC_SLOT 3                      /* 0x180 SError      */

    /* Current EL, SP_ELx: the kernel's own faults. */
    VEC_SLOT 4                      /* 0x200 synchronous */
    IRQ_SLOT                        /* 0x280 IRQ         */
    VEC_SLOT 6                      /* 0x300 FIQ         */
    VEC_SLOT 7                      /* 0x380 SError      */

    /* Lower EL in AArch64: EL0. */
    .balign 128
    b       kickos_armv8a_el0_sync_entry    /* 0x400 synchronous: the syscall trap */
    .balign 128
    b       kickos_armv8a_el0_irq_entry     /* 0x480 IRQ: an EL0 thread preempted */
    LOWER_EL_SLOT 10                /* 0x500 FIQ         */
    LOWER_EL_SLOT 11                /* 0x580 SError      */

    /* Lower EL in AArch32, which this port never enters. */
    LOWER_EL_SLOT 12                /* 0x600 synchronous */
    LOWER_EL_SLOT 13                /* 0x680 IRQ         */
    LOWER_EL_SLOT 14                /* 0x700 FIQ         */
    LOWER_EL_SLOT 15                /* 0x780 SError      */

    .end
