/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * nRF51822 (BBC micro:bit v1, Cortex-M0) startup: the vector table. Cortex-M0 has no VTOR, so
 * the table must live at flash 0x0.
 */

#include <chip_layout.h>
#include <kickos/chip_limits.h>
#include "core_vectors.inc"

    .syntax unified
    .thumb
    .cpu cortex-m0

    .section .isr_vector, "a"
    .align 2
    .global g_isr_vector
    .type g_isr_vector, %object
g_isr_vector:
    KICKOS_CORE_VECTORS_V6M

    /* Every line to the generic IRQ-as-event entry, except TIMER0's, which carries the
     * monotonic-clock wrap observer (chip_nrf51.cc). */
    .rept KICKOS_LAYOUT_LINE_TIMER0_IRQ
    .word kickos_arm_default_irq
    .endr
    .word kickos_nrf51_clock_isr     /* TIMER0 COMPARE[3] (clock wrap) */
    .rept KICKOS_MAX_IRQ - KICKOS_LAYOUT_LINE_TIMER0_IRQ - 1
    .word kickos_arm_default_irq
    .endr
    .size g_isr_vector, . - g_isr_vector

    .text
    .thumb_func
NMI_Handler:
    b       .

    .end
