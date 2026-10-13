/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the AT91SAM3X8E (Arduino Due), Cortex-M3.
 *   Flash : 512 KiB @ 0x0008_0000  (Flash 0; aliased to 0x0 at boot. VTOR is set
 *                                   to this base in the reset handler.)
 *   SRAM  :  96 KiB @ 0x2007_0000  (the contiguous alias: SRAM0 64 KiB + SRAM1 32 KiB
 *                                   joined at 0x2007_0000..0x2008_8000, so both banks
 *                                   are usable as one region, NOT 64 KiB @ 0x2000_0000)
 */

#include <chip_layout.h>
#include <cortexm.ld.h>

ENTRY(Reset_Handler)

MEMORY
{
    KICKOS_LINK_FLASH
    KICKOS_LINK_RAM
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
_kernel_stack_size = 8K;

SECTIONS
{
    .text :
    {
        KEEP(*(.isr_vector))
        KICKOS_CORTEXM_TEXT
    } > FLASH

    KICKOS_CORTEXM_CTORS
    KICKOS_CORTEXM_EH
    KICKOS_CORTEXM_EXIDX
    KICKOS_CORTEXM_DATA()
    KICKOS_CORTEXM_DISCARD
}

KICKOS_CORTEXM_ASSERTS()
KICKOS_KERNEL_CTORS_NONE_ASSERT()
KICKOS_APP_CODE_WINDOW_NONE()

#include <boot_arena.ld.h>
KICKOS_BOOT_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)
KICKOS_POOL_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)
