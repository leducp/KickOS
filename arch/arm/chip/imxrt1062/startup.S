/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * i.MX RT1062 (Teensy 4.1) startup: the boot entry trampoline and the reset
 * vector table (16 core + 160 NVIC lines; RT1060 CM7 has IRQ0..159, RM ch.4).
 * The kernel exception handlers (SVC/PendSV/SysTick) come from the armv7m arch
 * layer; every external line routes to the generic kickos_armv7m_default_irq
 * (line = IPSR-16). The FlexSPI boot header (FCB/IVT/Boot Data) is C data in
 * chip_imxrt1062.cc.
 */

#include <kickos/board_config.h>   /* KICKOS_MAX_IRQ: the NVIC line count */

    .syntax unified
    .cpu cortex-m7
    .thumb

    /* Boot entry: the FlexSPI boot ROM jumps here from IVT.entry (RM 9.7.1),
       NOT via the reset vector, so hardware has NOT loaded MSP from the vector
       table. Set MSP to the top of RAM before running the C reset path (which
       uses the stack), then hand off to the C Reset_Handler (never returns). */
    .section .text._boot_entry, "ax"
    .thumb_func
    .global _boot_entry
_boot_entry:
    ldr     sp, =_estack
    ldr     r0, =Reset_Handler
    bx      r0

    .section .isr_vector, "a"
    .align 10
    .global g_isr_vector
g_isr_vector:
    .word _estack                    /* 0: initial MSP (warm reset via VTOR) */
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

    /* External interrupts 0..(KICKOS_MAX_IRQ-1) -> the generic IRQ-as-event
       entry. The .rept count and the kernel IRQ table are ONE fact (MAX_IRQ). */
    .rept KICKOS_MAX_IRQ
    .word kickos_armv7m_default_irq
    .endr

    .text
    .thumb_func
    .weak NMI_Handler
NMI_Handler:
    b       .

    .end
