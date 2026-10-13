/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the STM32F103C8, Cortex-M3: 64 KiB flash / 20 KiB SRAM, the
 * genuine medium-density part. A low-density clone (32 KiB / 10 KiB, DEV_ID 0x412)
 * is a DIFFERENT part and would need its own chip entry: at 20 KiB the initial SP lands
 * past a 10 KiB die and the first push hardfaults before main.
 *   Flash : 64 KiB @ 0x0800_0000  (aliased to 0x0 at boot; vectors live here)
 *   SRAM  : 20 KiB @ 0x2000_0000
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
_kernel_stack_size = 2K;

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
