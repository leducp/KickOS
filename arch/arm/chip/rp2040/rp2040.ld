/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the Raspberry Pi RP2040, Cortex-M0+. Code executes in place (XIP) from
 * external QSPI flash at 0x1000_0000, whose size is the board file's; the first 256 bytes are the
 * second-stage bootloader (boot2, .boot2 section, CRC-checked by the ROM), and the vector table
 * follows at 0x1000_0100. SRAM is 264 KiB @ 0x2000_0000.
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
_kernel_stack_size = 8K;

/* PMSA needs the app window pow2-sized and aligned to its size. Its pad is the newlib heap:
   a 32K window leaves about 14K, room for the cxxtest and printf peaks of about 5.9K each. */
#if KICKOS_HAVE_MPU
#ifndef KICKOS_APPDATA_SIZE
#define KICKOS_APPDATA_SIZE 32K
#endif
_appdata_size = KICKOS_APPDATA_SIZE;
#ifndef KICKOS_KERNEL_DATA_RESERVE
#define KICKOS_KERNEL_DATA_RESERVE 32K
#endif
KICKOS_KERNEL_DATA_RESERVE_DECL(ORIGIN(RAM))
#endif

SECTIONS
{
    /* boot2: exactly 256 bytes at flash start (the ROM loads + CRC-checks it). */
    .boot2 ORIGIN(FLASH) :
    {
        KEEP(*(.boot2))
    } > FLASH

    /* The vector table is the first thing in .text, so pinning .text pins the table. */
    .text ORIGIN(FLASH) + 0x100 :
    {
        KEEP(*(.isr_vector))
        KICKOS_CORTEXM_TEXT
    } > FLASH

    KICKOS_CORTEXM_EH
    KICKOS_CORTEXM_CTORS
    KICKOS_CORTEXM_EXIDX
    KICKOS_INIT_TABLES(FLASH)
    KICKOS_CORTEXM_DATA(KICKOS_APPDATA_SECTION(_ebss))

    /* The whole flash, one pow2 PMSA region: an unprivileged thread fetches its code and reads
       flash rodata, kernel code included. */
    __kickos_code_start = ORIGIN(FLASH);
    __kickos_code_end = ORIGIN(FLASH) + LENGTH(FLASH);

    KICKOS_CORTEXM_DISCARD
}

KICKOS_CORTEXM_ASSERTS()

#include <boot_arena.ld.h>
KICKOS_BOOT_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)
KICKOS_POOL_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)

ASSERT(ADDR(.text) == ORIGIN(FLASH) + 0x100,
       "KickOS: RP2040 vector table must be at 0x1000_0100 (after boot2)")
