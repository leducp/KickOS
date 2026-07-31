/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * mps2-an386 (QEMU Cortex-M4F) startup: the reset vector table. vector[0] = the
 * initial MSP (_estack), vector[1] = Reset_Handler. The kernel exception
 * handlers (SVC/PendSV/SysTick) come from the armv7m arch layer; every external
 * NVIC line routes to the generic kickos_armv7m_default_irq (line = IPSR-16).
 */

#include <kickos/board_config.h>   /* KICKOS_MAX_IRQ: the NVIC line count */
#include "core_vectors.inc"        /* shared core-exception vector block */

    .syntax unified
    .thumb

    .section .isr_vector, "a"
    .align 8                         /* 256-byte aligned (VTOR requirement) */
    .global g_isr_vector
g_isr_vector:
    KICKOS_CORE_VECTORS_V7M

    /* External interrupts 0..47 -> the generic IRQ-as-event entry. */
    .rept KICKOS_MAX_IRQ
    .word kickos_armv7m_default_irq
    .endr

    .text
    .thumb_func
    /* File-local: the vector word above is the only reference, and a chip that wants a
     * real NMI handler edits this startup.S. */
NMI_Handler:
    b       .

    .end
