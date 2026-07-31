/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * STM32F103C8 ("Blue Pill", Cortex-M3) startup: reset vector table (16 core + 43
 * NVIC lines, medium-density F103). SVC/PendSV/SysTick come from the armv7m arch
 * layer; every external line routes to kickos_armv7m_default_irq (line=IPSR-16).
 * Runs from internal flash at 0x0800_0000 (aliased to 0x0 at boot); no FPU (M3),
 * no flash config field.
 */

#include <kickos/board_config.h>   /* KICKOS_MAX_IRQ: the NVIC line count */
#include "core_vectors.inc"        /* shared core-exception vector block */

    .syntax unified
    .thumb

    .section .isr_vector, "a"
    .align 9
    .global g_isr_vector
g_isr_vector:
    KICKOS_CORE_VECTORS_V7M

    /* External interrupts 0..42 -> the generic IRQ-as-event entry, except NVIC 29
     * (TIM3) which carries the monotonic-clock wrap observer (chip_stm32f103.cc). */
    .rept 29
    .word kickos_armv7m_default_irq
    .endr
    .word kickos_tim3_clock_isr      /* 29: TIM3 update (clock-chain wrap) */
    .rept (KICKOS_MAX_IRQ - 30)
    .word kickos_armv7m_default_irq
    .endr

    .text
    .thumb_func
    /* File-local: the vector word above is the only reference, and a chip that wants a
     * real NMI handler edits this startup.S. */
NMI_Handler:
    b       .

    .end
