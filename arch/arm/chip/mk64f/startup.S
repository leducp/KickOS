/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * MK64FN1M0 (FRDM-K64F) startup: reset vector table (16 core + 86 NVIC lines,
 * K64 has IRQ0..85) and the Flash Configuration Field. The kernel exception
 * handlers (SVC/PendSV/SysTick) come from the armv7m arch layer; every external
 * line routes to the generic kickos_armv7m_default_irq (line = IPSR-16).
 */

#include <kickos/chip_limits.h>   /* KICKOS_MAX_IRQ: the NVIC line count */
#include "core_vectors.inc"        /* shared core-exception vector block */

    .syntax unified
    .thumb

    .section .isr_vector, "a"
    .align 9
    .global g_isr_vector
g_isr_vector:
    KICKOS_CORE_VECTORS_V7M

    /* External interrupts 0..85 -> the generic IRQ-as-event entry. */
    .rept KICKOS_MAX_IRQ
    .word kickos_armv7m_default_irq
    .endr

    /* Flash Configuration Field @ 0x400 (placed by mk64f.ld .fcf). All-ones
       except FSEC = 0xFE (unsecured: SEC=10, MEEN/FSLACC/KEYEN=11) at byte
       0x40C, so word[3] = 0xFFFFFFFE. Zeroing this bricks debug access. */
    .section .fcf, "a"
    .align 2
    .word 0xFFFFFFFF                 /* 0x400: Backdoor key [0..3] */
    .word 0xFFFFFFFF                 /* 0x404: Backdoor key [4..7] */
    .word 0xFFFFFFFF                 /* 0x408: FPROT0-3 (no protection) */
    .word 0xFFFFFFFE                 /* 0x40C: FSEC=0xFE, FOPT/FEPROT/FDPROT=0xFF */

    .text
    .thumb_func
    /* File-local: the vector word above is the only reference, and a chip that wants a
     * real NMI handler edits this startup.S. */
NMI_Handler:
    b       .

    .end
