/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Node 1's vector table, and the park handler every line but the doorbell points at.
 *
 * The reset table (startup.S) funnels every external line into kickos_armv7m_default_irq,
 * which reaches kernel() through kickos_instance_index(); node 1 runs no scheduler, and with
 * multi-instance off that Kernel is core 0's LIVE state. N5's launch hands core 1 its own
 * VTOR, so this table closes that path structurally.
 */

#include <kickos/chip_limits.h>

#if KICKOS_NUM_CORES > 1

    .syntax unified
    .thumb
    .cpu cortex-m33

    .section .node_vector, "a"
    /* Armv8-M VTOR ignores the low bits: a table is aligned to its own size rounded up to a
       power of two, and to at least 128 bytes. 16 + KICKOS_MAX_IRQ entries is 272 bytes. */
    .align 9
    .global g_node_isr_vector
    .type g_node_isr_vector, %object
g_node_isr_vector:
    /* Slots 0 and 1 are the reset SP and PC, which the exception mechanism never fetches from
       a relocated table: the RP2350 launch hands core 1 its stack and entry point over the
       inter-processor FIFO (datasheet RP-008373-DS-2, 5.3). */
    .rept 16 + KICKOS_RP2350_SIO_IRQ_BELL
    .word kickos_rp2350_node_park
    .endr
    .word kickos_rp2350_doorbell_service
    .rept KICKOS_MAX_IRQ - KICKOS_RP2350_SIO_IRQ_BELL - 1
    .word kickos_rp2350_node_park
    .endr
    .size g_node_isr_vector, . - g_node_isr_vector

    /* Terminal: the exception is never returned from, so a line this node has no business
       taking cannot reach thread context either. */
    .section .text.kickos_rp2350_node_park, "ax", %progbits
    .thumb_func
    .global kickos_rp2350_node_park
kickos_rp2350_node_park:
    cpsid   i
1:  wfi
    b       1b
    .size kickos_rp2350_node_park, . - kickos_rp2350_node_park

#endif
