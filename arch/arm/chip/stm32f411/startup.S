/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * STM32F411 (Cortex-M4F) startup: reset vector table (16 core + 86 NVIC lines).
 * The kernel exception handlers (SVC/PendSV/SysTick) come from the armv7m arch
 * layer; every external line routes to the generic kickos_armv7m_default_irq
 * (line = IPSR-16). STM32 runs from internal flash at 0x0800_0000 (aliased to
 * 0x0 at boot); the reset handler sets VTOR (in the chip C) is unnecessary since
 * M4 boots with VTOR=0 pointing at the flash-alias table -- the linker places
 * .isr_vector at flash start. There is no flash config field (that is K64F).
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

    /* External interrupts 0..85 -> the generic IRQ-as-event entry, except NVIC 28
     * (TIM2) which carries the monotonic-clock wrap observer (chip_stm32f411.cc). */
    .rept 28
    .word kickos_armv7m_default_irq
    .endr
    .word kickos_tim2_clock_isr      /* 28: TIM2 update (clock wrap) */
    .rept (KICKOS_MAX_IRQ - 29)
    .word kickos_armv7m_default_irq
    .endr

    .text
    .thumb_func
    /* Only reference is the vector word above; override here for a real handler. */
NMI_Handler:
    b       .

    .end
