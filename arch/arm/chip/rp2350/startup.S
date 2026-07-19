/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Raspberry Pi RP2350 (Cortex-M33, Secure state) startup: the application reset
 * vector table PLUS the boot IMAGE_DEF block. Both live in flash at the XIP image
 * base 0x1000_0000 (see rp2350.ld).
 *
 * Unlike the RP2040, there is NO 256-byte checksummed boot2 at flash 0 and no
 * SSI/XIP setup stage: the RP2350 bootrom does its own best-effort QSPI XIP setup
 * during the flash scan, then enters the image (datasheet 5.9.5). What the image
 * MUST carry instead is an IMAGE_DEF block somewhere in the first 4 KiB
 * (datasheet 5.1.5.2, 5.9.5): the bootrom scans for the block-loop start marker,
 * validates the IMAGE_DEF, then -- with no ENTRY_POINT/VECTOR_TABLE item present
 * -- reads the initial SP from [base+0] and the reset PC from [base+4] of the
 * vector table at the image base (datasheet 5.9.3.3, 5.9.5.1). So the vector
 * table is PINNED at 0x1000_0000 and the IMAGE_DEF sits immediately after it.
 *
 * SVC/PendSV/SysTick come from the armv7m arch layer (armv8-m reuses the v7-m
 * thread/switch/NVIC path); each external line routes to the generic
 * kickos_armv7m_default_irq (line = IPSR-16). The RP2350 wires 52 NVIC inputs
 * (IRQ0..51; 46..51 are spare/never-fire), matching KICKOS_MAX_IRQ.
 */

#include <kickos/board_config.h>   /* KICKOS_MAX_IRQ: the NVIC line count */

    .syntax unified
    .thumb
    .cpu cortex-m33

    .section .isr_vector, "a"
    .align 9
    .global g_isr_vector
g_isr_vector:
    .word _estack                    /* 0: initial MSP (bootrom reads [base+0]) */
    .word Reset_Handler              /* 1: reset      (bootrom reads [base+4]) */
    .word NMI_Handler                /* 2 */
    .word HardFault_Handler          /* 3 */
    .word HardFault_Handler          /* 4: MemManage */
    .word HardFault_Handler          /* 5: BusFault */
    .word HardFault_Handler          /* 6: UsageFault */
    .word HardFault_Handler          /* 7: SecureFault (armv8-m) */
    .word 0                          /* 8 */
    .word 0                          /* 9 */
    .word 0                          /* 10 */
    .word SVC_Handler                /* 11: SVCall */
    .word 0                          /* 12: DebugMonitor */
    .word 0                          /* 13 */
    .word PendSV_Handler             /* 14: PendSV */
    .word SysTick_Handler            /* 15: SysTick */

    /* External interrupts 0..51 -> the generic IRQ-as-event entry. */
    .rept KICKOS_MAX_IRQ
    .word kickos_armv7m_default_irq
    .endr

    /* --- Boot IMAGE_DEF block (datasheet 5.9.5.1 "Minimum Arm IMAGE_DEF") ------
     * The verbatim 20-byte block the RP2350 bootrom accepts for an UNSIGNED image.
     * On a chip that is not secured (CRIT1.SECURE_BOOT_ENABLE clear) no SIGNATURE/
     * HASH_DEF item is required. The image_type_flags word (0x1021) marks:
     *   EXE(1) | Security=S(2) | CPU=ARM(0) | CHIP=RP2350(1).
     * Security=S matches the state the M33 boots in (we do not use TrustZone / drop
     * to Non-secure). The block links to itself (LINK=0) as a single-block loop.
     * Kept because nothing references it: KEEP(*(.image_def)) in rp2350.ld, and the
     * object is already pulled in via the -u,g_isr_vector force-ref. */
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
    .weak NMI_Handler
NMI_Handler:
    b       .

    .end
