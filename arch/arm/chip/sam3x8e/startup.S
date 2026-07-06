/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * AT91SAM3X8E (Arduino Due, Cortex-M3) startup: reset vector table (16 core + 45
 * peripheral lines, IDs 0..44). SVC/PendSV/SysTick come from the armv7m arch
 * layer; every external line routes to kickos_armv7m_default_irq (line=IPSR-16).
 * The table is linked at the real flash base 0x0008_0000 (aliased to 0x0 at
 * boot); chip_sam3x8e's Reset_Handler points VTOR at it.
 */

#include <kickos/board_config.h>   /* KICKOS_MAX_IRQ: the NVIC line count */

    .syntax unified
    .thumb

    .section .isr_vector, "a"
    .align 9
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
    .word 0                          /* 12: DebugMonitor */
    .word 0                          /* 13 */
    .word PendSV_Handler             /* 14: PendSV */
    .word SysTick_Handler            /* 15: SysTick */

    /* Peripheral interrupts 0..44 -> the generic IRQ-as-event entry. */
    .rept KICKOS_MAX_IRQ
    .word kickos_armv7m_default_irq
    .endr

    .text
    .thumb_func
    .weak NMI_Handler
NMI_Handler:
    b       .

    .end
