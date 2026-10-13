/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Infineon XMC4800 (Cortex-M4F) startup: the vector table, linked at the cached flash alias
 * 0x0800_0000; chip_xmc4800 sets VTOR to it.
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

    .text
    .thumb_func
NMI_Handler:
    b       .

    .end
