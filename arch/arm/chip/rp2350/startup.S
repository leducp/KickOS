/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Raspberry Pi RP2350 (Cortex-M33, Secure state) startup: the vector table PLUS the boot
 * IMAGE_DEF block, both at the XIP image base 0x1000_0000.
 *
 * There is no boot2 stage: the bootrom does its own QSPI XIP setup, scans the first 4 KiB for an
 * IMAGE_DEF block, and with no ENTRY_POINT/VECTOR_TABLE item reads the initial SP and reset PC
 * from the vector table at the image base (datasheet 5.9.3.3, 5.9.5.1). So the table is PINNED
 * at 0x1000_0000 and the IMAGE_DEF follows it.
 *
 * The chip wires 52 NVIC inputs (IRQ0..51; 46..51 are spare), matching KICKOS_MAX_IRQ.
 */

#include <chip_layout.h>
#include <kickos/chip_limits.h>
#include "core_vectors.inc"

    .syntax unified
    .thumb
    .cpu cortex-m33

    .section .isr_vector, "a"
    .align 9
    .global g_isr_vector
g_isr_vector:
    KICKOS_CORE_VECTORS_V8M

#if (KICKOS_NUM_CORES > 1 || KICKOS_AMP_NODE)
    /* SIO_IRQ_BELL is dispatched here and never through kickos_isr_irq, so it occupies no
       kernel irq_table slot. KICKOS_KERNEL_LINES (CMakeLists.txt) carries it to irq_claim
       under the same condition, which refuses a capability over the line. */
    .rept KICKOS_LAYOUT_LINE_SIO_IRQ_BELL
    .word kickos_arm_default_irq
    .endr
    .word kickos_rp2350_doorbell_service
    .rept KICKOS_MAX_IRQ - KICKOS_LAYOUT_LINE_SIO_IRQ_BELL - 1
    .word kickos_arm_default_irq
    .endr
#else
    .rept KICKOS_MAX_IRQ
    .word kickos_arm_default_irq
    .endr
#endif

    /* The datasheet 5.9.5.1 "Minimum Arm IMAGE_DEF": the 20-byte block the bootrom accepts for
     * an UNSIGNED image on a chip with CRIT1.SECURE_BOOT_ENABLE clear. image_type_flags 0x1021 =
     * EXE | Security=S (the M33 boots Secure; no TrustZone) | CPU=ARM | CHIP=RP2350. LINK=0 makes
     * it a single-block loop. Nothing references it: KEEP(*(.image_def)) in rp2350.ld retains
     * it, and the -u,g_isr_vector force-ref pulls this object. */
    .section .image_def, "a"
    .align 2
    .global g_image_def
g_image_def:
    .word 0xffffded3                 /* PICOBIN_BLOCK_MARKER_START */
    .word 0x10210142                 /* IMAGE_DEF item: type=0x42, 1 word, flags=0x1021 */
    .word 0x000001ff                 /* LAST item: size=1 word */
    .word 0x00000000                 /* LINK: 0 = self-loop (single block) */
    .word 0xab123579                 /* PICOBIN_BLOCK_MARKER_END */

    .text
    .thumb_func
NMI_Handler:
    b       .

    .end
