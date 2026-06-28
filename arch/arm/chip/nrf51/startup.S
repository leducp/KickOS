/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * nRF51822 (BBC micro:bit v1, Cortex-M0) startup: reset vector table. Cortex-M0
 * has no VTOR, so the table must live at flash 0x0 (linker puts .isr_vector at
 * the start of .text). SVC/PendSV/SysTick come from the armv6m arch layer; each
 * external line routes to the generic kickos_armv6m_default_irq (line=IPSR-16).
 */

    .syntax unified
    .thumb
    .cpu cortex-m0

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
    .rept 32
    .word kickos_armv6m_default_irq
    .endr

    .text
    .thumb_func
    .weak NMI_Handler
NMI_Handler:
    b       .

    .end
