/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * i.MX RT1062 (Teensy 4.1) startup: the boot entry trampoline and the vector table. The FlexSPI
 * boot header is C data in chip_imxrt1062.cc.
 */

#include <chip_layout.h>
#include <kickos/chip_limits.h>
#include "core_vectors.inc"

    .syntax unified
    .cpu cortex-m7
    .thumb

    /* The FlexSPI boot ROM jumps here from IVT.entry (RM 9.7.1), not via the reset vector, so
       MSP is NOT loaded from the table. */
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
    .type g_isr_vector, %object
g_isr_vector:
    KICKOS_CORE_VECTORS_V7M

    /* External lines -> the generic IRQ-as-event entry, except GPT1's, which carries the
     * monotonic-clock wrap (chip_imxrt1062.cc). The total and the kernel IRQ table are ONE
     * fact (MAX_IRQ). */
    .rept KICKOS_LAYOUT_LINE_GPT1_IRQ
    .word kickos_arm_default_irq
    .endr
    .word kickos_gpt1_clock_isr      /* GPT1 rollover (clock wrap) */
    .rept KICKOS_MAX_IRQ - KICKOS_LAYOUT_LINE_GPT1_IRQ - 1
    .word kickos_arm_default_irq
    .endr
    .size g_isr_vector, . - g_isr_vector

    .text
    .thumb_func
NMI_Handler:
    b       .

    .end
