/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Infineon XMC4800 (Cortex-M4F) startup: reset vector table (16 core + 112
 * interrupt nodes, IRQ0..IRQ111). SVC/PendSV/SysTick come from the armv7m arch
 * layer; every external line routes to kickos_armv7m_default_irq (line=IPSR-16).
 * Linked at the cached flash alias 0x0800_0000; chip_xmc4800 sets VTOR to it.
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

    /* Interrupt nodes IRQ0..IRQ111 -> the generic IRQ-as-event entry. */
    .rept KICKOS_MAX_IRQ
    .word kickos_armv7m_default_irq
    .endr

    .text
    .thumb_func
    .weak NMI_Handler
NMI_Handler:
    b       .

    .end
