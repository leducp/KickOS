/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * nRF51822 (BBC micro:bit v1, Cortex-M0) startup: the vector table. Cortex-M0 has no VTOR, so
 * the table must live at flash 0x0.
 */

#include <kickos/chip_limits.h>
#include "core_vectors.inc"

    .syntax unified
    .thumb
    .cpu cortex-m0

    .section .isr_vector, "a"
    .align 2
    .global g_isr_vector
g_isr_vector:
    KICKOS_CORE_VECTORS_V6M

    .rept KICKOS_MAX_IRQ
    .word kickos_arm_default_irq
    .endr

    .text
    .thumb_func
NMI_Handler:
    b       .

    .end
