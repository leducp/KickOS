/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Raspberry Pi RP2040 (Pico), Cortex-M0+ startup: the vector table, at flash 0x1000_0100
 * after the 256-byte boot2 (rp2040.ld). boot2 points VTOR here and loads the initial MSP from
 * word 0 before branching to Reset_Handler.
 *
 * The chip wires 26 NVIC lines (IRQ0..25); the table is padded to KICKOS_MAX_IRQ.
 */

#include <kickos/chip_limits.h>
#include "core_vectors.inc"

    .syntax unified
    .thumb
    .cpu cortex-m0plus

    .section .isr_vector, "a"
    .align 2
    .global g_isr_vector
g_isr_vector:
    KICKOS_CORE_VECTORS_V6M

    .rept KICKOS_MAX_IRQ
    .word kickos_arm_default_irq
    .endr

    .text
    .thumb_func
NMI_Handler:
    b       .

    .end
