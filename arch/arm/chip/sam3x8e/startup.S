/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * AT91SAM3X8E (Arduino Due, Cortex-M3) startup: reset vector table (16 core + 45
 * peripheral lines, IDs 0..44). SVC/PendSV/SysTick come from the armv7m arch
 * layer; every external line routes to kickos_armv7m_default_irq (line=IPSR-16).
 * The table is linked at the real flash base 0x0008_0000 (aliased to 0x0 at
 * boot); chip_sam3x8e's Reset_Handler points VTOR at it.
 */

#include <kickos/chip_limits.h>   /* KICKOS_MAX_IRQ: the NVIC line count */
#include "core_vectors.inc"        /* shared core-exception vector block */

    .syntax unified
    .thumb

    .section .isr_vector, "a"
    .align 9
    .global g_isr_vector
g_isr_vector:
    KICKOS_CORE_VECTORS_V7M

    /* Peripheral interrupts 0..44 -> the generic IRQ-as-event entry, except line 27
     * (TC0 ch0) which carries the monotonic-clock wrap observer (chip_sam3x8e.cc). */
    .rept 27
    .word kickos_armv7m_default_irq
    .endr
    .word kickos_tc0_clock_isr       /* 27: TC0 ch0 overflow (clock wrap) */
    .rept (KICKOS_MAX_IRQ - 28)
    .word kickos_armv7m_default_irq
    .endr

    .text
    .thumb_func
    /* File-local: the vector word above is the only reference, and a chip that wants a
     * real NMI handler edits this startup.S. */
NMI_Handler:
    b       .

    .end
