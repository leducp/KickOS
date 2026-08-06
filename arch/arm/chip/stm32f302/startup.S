/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * STM32F302R8 (Cortex-M4F) startup: reset vector table (16 core + 82 NVIC lines;
 * the F302x6/8 vector table tops out at position 81 = FPU). SVC/PendSV/SysTick
 * come from the armv7m arch layer; every external line routes to
 * kickos_armv7m_default_irq (line = IPSR-16). Runs from flash at 0x0800_0000.
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

    /* External interrupts 0..81 -> the generic IRQ-as-event entry, except NVIC 28
     * (TIM2) which carries the monotonic-clock wrap observer (chip_stm32f302.cc). */
    .rept 28
    .word kickos_armv7m_default_irq
    .endr
    .word kickos_tim2_clock_isr      /* 28: TIM2 update (clock wrap) */
    .rept (KICKOS_MAX_IRQ - 29)
    .word kickos_armv7m_default_irq
    .endr

    .text
    .thumb_func
    /* File-local: the vector word above is the only reference, and a chip that wants a
     * real NMI handler edits this startup.S. */
NMI_Handler:
    b       .

    .end
