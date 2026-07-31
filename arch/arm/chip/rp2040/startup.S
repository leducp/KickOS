/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Raspberry Pi RP2040 (Pico), Cortex-M0+ startup: the application reset vector
 * table. It sits at flash 0x1000_0100 (immediately after the 256-byte boot2, see
 * rp2040.ld); boot2 points VTOR here and loads the initial MSP from word 0 before
 * branching to Reset_Handler. SVC/PendSV/SysTick come from the armv6m arch layer;
 * each external line routes to the generic kickos_armv6m_default_irq (line=IPSR-16).
 *
 * The RP2040 wires 26 NVIC lines (IRQ0..25); the table is padded to 32 to match
 * KICKOS_MAX_IRQ for this board (the in-kernel IRQ table size).
 */

#include <kickos/board_config.h>   /* KICKOS_MAX_IRQ: the NVIC line count */
#include "core_vectors.inc"        /* shared core-exception vector block */

    .syntax unified
    .thumb
    .cpu cortex-m0plus

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
    /* File-local: the vector word above is the only reference, and a chip that wants a
     * real NMI handler edits this startup.S. */
NMI_Handler:
    b       .

    .end
