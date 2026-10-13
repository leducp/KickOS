/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the QEMU `mps2-an386` (ARM MPS2 FPGA, Cortex-M4F), the runnable armv7m
 * CI target. QEMU maps a code region at 0x00000000 (vectors read at reset, VTOR=0) and SRAM at
 * 0x20000000. `qemu-m7` (mps2-an500) and `qemu-m3` (mps2-an385) share this map.
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

/* PMSA needs the app window pow2-sized and aligned to its size. Its pad is the newlib heap:
   64K covers the selftest image's app-side .bss plus the cxxtest heap peak. */
#if KICKOS_HAVE_MPU
#ifndef KICKOS_APPDATA_SIZE
#define KICKOS_APPDATA_SIZE 64K
#endif
_appdata_size = KICKOS_APPDATA_SIZE;
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
    KICKOS_CORTEXM_DATA(KICKOS_APPDATA_SECTION(_ebss))

#if KICKOS_HAVE_MPU
    /* The whole flash, one pow2 PMSA region: an unprivileged thread fetches its code and reads
       flash rodata, kernel code included. */
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
