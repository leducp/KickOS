/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the QEMU `mps2-an505` (ARM MPS2 FPGA, Cortex-M33), the runnable PMSAv8
 * target.
 *
 * The AN505 comes out of reset in SECURE state, so the image links at the SECURE aliases of
 * the two SSRAM banks: code SRAM at 0x1000_0000 (4 MiB) and SSRAM1 at 0x3800_0000 (2 MiB). At
 * the non-secure aliases every access would be on the far side of the SAU/IDAU from the
 * state the core runs in.
 */

#include <chip_layout.h>
#include <cortexm.ld.h>
#include <kernel_data_reserve.ld.h>

ENTRY(Reset_Handler)

MEMORY
{
    KICKOS_LINK_FLASH
    KICKOS_LINK_RAM
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
_kernel_stack_size = 16K;

/* Kernel .data/.bss below the images' windows (kernel_data_reserve.ld.h). */
#if KICKOS_HAVE_MPU
#ifndef KICKOS_KERNEL_DATA_RESERVE
#define KICKOS_KERNEL_DATA_RESERVE 64K
#endif
KICKOS_KERNEL_DATA_RESERVE_DECL(ORIGIN(RAM))
#endif

SECTIONS
{
    .isr_vector ORIGIN(FLASH) :
    {
        KEEP(*(.isr_vector))
    } > FLASH

    .text :
    {
        KICKOS_CORTEXM_TEXT
    } > FLASH

    KICKOS_CORTEXM_CTORS
    KICKOS_CORTEXM_EH
    KICKOS_CORTEXM_EXIDX
    KICKOS_INIT_TABLES(FLASH)
    KICKOS_CORTEXM_DATA()

#if KICKOS_HAVE_MPU
    /* The whole flash as the app's RX region, kernel code included. */
    __kickos_code_start = ORIGIN(FLASH);
    __kickos_code_end = ORIGIN(FLASH) + LENGTH(FLASH);
#endif

    KICKOS_CORTEXM_DISCARD
}

KICKOS_CORTEXM_ASSERTS()
KICKOS_KERNEL_CTORS_NONE_ASSERT()

#include <boot_arena.ld.h>
KICKOS_BOOT_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)
KICKOS_POOL_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)

#if !KICKOS_HAVE_MPU
KICKOS_APP_CODE_WINDOW_NONE()
#endif
