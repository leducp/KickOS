/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * mps2-an386 (QEMU Cortex-M4F) startup: the reset vector table. vector[0] = the
 * initial MSP (_estack), vector[1] = Reset_Handler. The kernel exception
 * handlers (SVC/PendSV/SysTick) come from the armv7m arch layer; every external
 * NVIC line routes to the generic kickos_armv7m_default_irq (line = IPSR-16).
 */

#include <kickos/board_config.h>   /* KICKOS_MAX_IRQ: the NVIC line count */

    .syntax unified
    .thumb

    .section .isr_vector, "a"
    .align 8                         /* 256-byte aligned (VTOR requirement) */
    .global g_isr_vector
g_isr_vector:
    .word _estack                    /* 0: initial MSP */
    .word Reset_Handler              /* 1: reset */
    .word NMI_Handler                /* 2 */
    .word HardFault_Handler          /* 3 */
    .word HardFault_Handler          /* 4: MemManage */
    .word HardFault_Handler          /* 5: BusFault */
    .word HardFault_Handler          /* 6: UsageFault */
    .word 0                          /* 7 */
    .word 0                          /* 8 */
    .word 0                          /* 9 */
    .word 0                          /* 10 */
    .word SVC_Handler                /* 11: SVCall */
    .word 0                          /* 12: DebugMon */
    .word 0                          /* 13 */
    .word PendSV_Handler             /* 14: PendSV */
    .word SysTick_Handler            /* 15: SysTick */

    /* External interrupts 0..47 -> the generic IRQ-as-event entry. */
    .rept KICKOS_MAX_IRQ
    .word kickos_armv7m_default_irq
    .endr

    .text
    .thumb_func
    .weak NMI_Handler
NMI_Handler:
    b       .

    .end
