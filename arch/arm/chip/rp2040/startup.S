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

    .syntax unified
    .thumb
    .cpu cortex-m0plus

    .section .isr_vector, "a"
    .align 2
    .global g_isr_vector
g_isr_vector:
    .word _estack                    /* 0: initial MSP */
    .word Reset_Handler              /* 1: reset */
    .word NMI_Handler                /* 2 */
    .word HardFault_Handler          /* 3 */
    .word 0                          /* 4 */
    .word 0                          /* 5 */
    .word 0                          /* 6 */
    .word 0                          /* 7 */
    .word 0                          /* 8 */
    .word 0                          /* 9 */
    .word 0                          /* 10 */
    .word SVC_Handler                /* 11: SVCall */
    .word 0                          /* 12 */
    .word 0                          /* 13 */
    .word PendSV_Handler             /* 14: PendSV */
    .word SysTick_Handler            /* 15: SysTick */

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
