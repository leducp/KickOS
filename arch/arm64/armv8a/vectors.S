/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The AArch64 EL1 exception vector table. VBAR_EL1 holds its base, which the chip's
 * _start installs (arch/arm64/chip/virt_arm64/startup.S).
 *
 * The table's shape is architectural: 2 KiB aligned, sixteen slots of 0x80 bytes, in
 * four groups of four (synchronous, IRQ, FIQ, SError) by where the exception came from.
 * EVERY slot is filled, including the ones this stage cannot service, because the
 * alternative is not "nothing happens": an unfilled slot runs whatever bytes follow the
 * table, which is the silent death that has already cost this port one step. An
 * unexpected slot therefore reports which slot it was and stops.
 *
 * Nothing here returns, so a slot may clobber freely. It passes the slot index and the
 * interrupted x30, the two facts the reporter cannot recover for itself: everything else
 * it needs is in ESR_EL1, FAR_EL1, ELR_EL1 and SPSR_EL1.
 */

#include <kickos/board_config.h>

    .macro  VEC_SLOT index
    .balign 128
    mov     x0, #\index
    mov     x1, x30                 /* the interrupted LR, before any prologue saves it */
    b       kickos_armv8a_exception
    .endm

/* An IRQ slot RESUMES, so unlike the reporting slots it has to preserve everything a callee
 * may clobber, plus ELR_EL1 and SPSR_EL1: those two are the return address and processor
 * state the eret consumes, and any later exception would overwrite them. x19-x28 are
 * callee-saved, so the C dispatcher preserves those itself. The body lives in switch.S with
 * the frame it shares with the switcher: one format resumes a preempted thread and a thread
 * that switched of its own accord.
 */
    .macro  IRQ_SLOT
    .balign 128
    b       kickos_armv8a_irq_entry
    .endm

/* A lower-EL slot that only REPORTS still seats SP_EL1 from the kernel's own record. The
 * syscall and IRQ entries trust the register (switch.S), but they are the classes this port
 * enters: these slots are AArch32 entries and an EL0 SError, so reaching one means the core
 * was configured behind the kernel's back and SP_EL1 carries no promise. Unseated, the
 * reporter would build privileged frames on memory an unprivileged thread can write. Nothing
 * is preserved on this path, so it needs no scratch register.
 *
 * The block is per core and kernel_sp sits at displacement 0 (percpu.h, which static_asserts
 * it). At one core the array's first element IS this core's block, so the address is a
 * link-time constant; above one core it is TPIDR_EL1's value, seated on each core before that
 * core turns translation on (secondary.S, arch_armv8a.cc).
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
    /* Current EL, SP_EL0. SPSel comes out of reset selecting SP_ELx and nothing here
       clears it, so these four are unreachable-by-construction rather than unimplemented. */
    VEC_SLOT 0                      /* 0x000 synchronous */
    VEC_SLOT 1                      /* 0x080 IRQ         */
    VEC_SLOT 2                      /* 0x100 FIQ         */
    VEC_SLOT 3                      /* 0x180 SError      */

    /* Current EL, SP_ELx: the kernel's own faults. Slot 4 is the one a wild access in
       privileged code arrives at, and the only slot this stage services. */
    VEC_SLOT 4                      /* 0x200 synchronous */
    IRQ_SLOT                        /* 0x280 IRQ: the only interrupt this stage takes */
    VEC_SLOT 6                      /* 0x300 FIQ         */
    VEC_SLOT 7                      /* 0x380 SError      */

    /* Lower EL in AArch64: EL0. */
    .balign 128
    b       kickos_armv8a_el0_sync_entry    /* 0x400 synchronous: the syscall trap */
    .balign 128
    b       kickos_armv8a_el0_irq_entry     /* 0x480 IRQ: an EL0 thread preempted */
    LOWER_EL_SLOT 10                /* 0x500 FIQ         */
    LOWER_EL_SLOT 11                /* 0x580 SError      */

    /* Lower EL in AArch32, which this port never enters, so these mean the core was
       configured behind the kernel's back. */
    LOWER_EL_SLOT 12                /* 0x600 synchronous */
    LOWER_EL_SLOT 13                /* 0x680 IRQ         */
    LOWER_EL_SLOT 14                /* 0x700 FIQ         */
    LOWER_EL_SLOT 15                /* 0x780 SError      */

    .end
