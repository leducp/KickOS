/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * nRF51822 (BBC micro:bit v1, Cortex-M0) startup: reset vector table. Cortex-M0
 * has no VTOR, so the table must live at flash 0x0 (linker puts .isr_vector at
 * the start of .text). SVC/PendSV/SysTick come from the armv6m arch layer; each
 * external line routes to the generic kickos_armv6m_default_irq (line=IPSR-16).
 */

#include <kickos/board_config.h>   /* KICKOS_MAX_IRQ: the NVIC line count */
#include "core_vectors.inc"        /* shared core-exception vector block */

    .syntax unified
    .thumb
    .cpu cortex-m0

    .section .isr_vector, "a"
    .align 2
    .global g_isr_vector
g_isr_vector:
    KICKOS_CORE_VECTORS_V6M

    /* External interrupts 0..31 -> the generic IRQ-as-event entry. */
    .rept KICKOS_MAX_IRQ
    .word kickos_armv6m_default_irq
    .endr

    .text
    .thumb_func
    .weak NMI_Handler
NMI_Handler:
    b       .

    .end
