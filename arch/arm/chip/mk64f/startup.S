/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * MK64FN1M0 (FRDM-K64F) startup: the vector table and the Flash Configuration Field. Every
 * external line routes to kickos_arm_default_irq.
 */

#include <kickos/chip_limits.h>
#include "core_vectors.inc"

    .syntax unified
    .thumb

    .section .isr_vector, "a"
    .align 9
    .global g_isr_vector
g_isr_vector:
    KICKOS_CORE_VECTORS_V7M

    .rept KICKOS_MAX_IRQ
    .word kickos_arm_default_irq
    .endr

    /* Flash Configuration Field @ 0x400 (placed by mk64f.ld .fcf): all-ones except FSEC = 0xFE
       (unsecured). Zeroing this bricks debug access. */
    .section .fcf, "a"
    .align 2
    .word 0xFFFFFFFF                 /* 0x400: Backdoor key [0..3] */
    .word 0xFFFFFFFF                 /* 0x404: Backdoor key [4..7] */
    .word 0xFFFFFFFF                 /* 0x408: FPROT0-3 (no protection) */
    .word 0xFFFFFFFE                 /* 0x40C: FSEC=0xFE, FOPT/FEPROT/FDPROT=0xFF */

    .text
    .thumb_func
NMI_Handler:
    b       .

    .end
