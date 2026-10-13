/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * STM32F103C8 ("Blue Pill", Cortex-M3) startup: the medium-density vector table. Runs from
 * internal flash at 0x0800_0000 (aliased to 0x0 at boot).
 */

#include <chip_layout.h>
#include <kickos/chip_limits.h>
#include "core_vectors.inc"

    .syntax unified
    .thumb

    .section .isr_vector, "a"
    .align 9
    .global g_isr_vector
g_isr_vector:
    KICKOS_CORE_VECTORS_V7M

    /* External interrupts 0..42 -> the generic IRQ-as-event entry, except the TIM3
     * line, which carries the monotonic-clock wrap observer (chip_stm32f1f3.cc). */
    .rept KICKOS_LAYOUT_LINE_TIM3_GLOBAL
    .word kickos_arm_default_irq
    .endr
    .word kickos_stm32_clock_isr   /* TIM3 update (clock-chain wrap) */
    .rept KICKOS_MAX_IRQ - KICKOS_LAYOUT_LINE_TIM3_GLOBAL - 1
    .word kickos_arm_default_irq
    .endr

    .text
    .thumb_func
NMI_Handler:
    b       .

    .end
