/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * AT91SAM3X8E (Arduino Due, Cortex-M3) startup: the vector table (peripheral IDs 0..44), linked
 * at the real flash base 0x0008_0000 (aliased to 0x0 at boot); chip_sam3x8e's Reset_Handler
 * points VTOR at it.
 */

#include <chip_layout.h>
#include <kickos/chip_limits.h>
#include "core_vectors.inc"

    .syntax unified
    .thumb

    .section .isr_vector, "a"
    .align 9
    .global g_isr_vector
g_isr_vector:
    KICKOS_CORE_VECTORS_V7M

    /* Peripheral interrupts 0..44 -> the generic IRQ-as-event entry, except the TC0 ch0
     * line, which carries the monotonic-clock wrap observer (chip_sam3x8e.cc). */
    .rept KICKOS_LAYOUT_LINE_TC0_IRQ
    .word kickos_arm_default_irq
    .endr
    .word kickos_tc0_clock_isr       /* TC0 ch0 overflow (clock wrap) */
    .rept KICKOS_MAX_IRQ - KICKOS_LAYOUT_LINE_TC0_IRQ - 1
    .word kickos_arm_default_irq
    .endr

    .text
    .thumb_func
NMI_Handler:
    b       .

    .end
