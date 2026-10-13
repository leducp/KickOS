/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the NXP i.MX RT1062 (Teensy 4.1), Cortex-M7 (RM Rev. 3).
 *
 * FlexSPI serial-NOR XIP image: the boot ROM reads the FCB at flash offset 0 and the IVT at
 * 0x1000 (RM Table 9-36), and jumps to IVT.entry.
 *
 *   FLASH : FlexSPI serial NOR @ 0x6000_0000, its size the board file's  (RM 3.2)
 *   RAM   : OCRAM2 512 KiB @ 0x2020_0000                                  (RM 3.2)
 *
 * OCRAM2 is independent of the FlexRAM partition and available at reset, so the image needs
 * no FlexRAM programming.
 */

#include <chip_layout.h>
#include <cortexm.ld.h>
#include <kernel_data_reserve.ld.h>

ENTRY(_boot_entry)

MEMORY
{
    KICKOS_LINK_FLASH
    KICKOS_LINK_RAM
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
_kernel_stack_size = 8K;

/* PMSA needs the app window pow2-sized and its base ALIGNED to its size: the encoder checks
   the size only, and the hardware snaps a misaligned base DOWN onto kernel RAM. */
#if KICKOS_HAVE_MPU
#ifndef KICKOS_APPDATA_SIZE
#define KICKOS_APPDATA_SIZE 128K
#endif
_appdata_size = KICKOS_APPDATA_SIZE;
#ifndef KICKOS_KERNEL_DATA_RESERVE
#define KICKOS_KERNEL_DATA_RESERVE 128K
#endif
KICKOS_KERNEL_DATA_RESERVE_DECL(ORIGIN(RAM))
#endif

SECTIONS
{
    /* FlexSPI serial-NOR configuration block (FCB), 512 B at flash offset 0
       (RM 9.6.3.1 / Table 9-15, Table 9-18). ROM-consumed, so KEEP retains it. */
    .boot_fcb ORIGIN(FLASH) :
    {
        KEEP(*(.boot_fcb))
    } > FLASH

    /* Image Vector Table @ 0x1000 and Boot Data @ 0x1020 (RM 9.7.1, Table 9-36:
       FlexSPI NOR IVT offset = 0x1000). Both ROM-consumed. */
    .boot_ivt ORIGIN(FLASH) + 0x1000 :
    {
        KEEP(*(.boot_ivt))
        KEEP(*(.boot_data))
    } > FLASH

    /* After the 8 KiB boot-header window. _boot_entry points VTOR here: the ROM enters
       through IVT.entry, not the reset vector. */
    .isr_vector ORIGIN(FLASH) + 0x2000 :
    {
        . = ALIGN(1024);   /* M7 VTOR alignment: >= table size, 1 KiB is ample */
        KEEP(*(.isr_vector))
    } > FLASH

    .text :
    {
        KICKOS_CORTEXM_TEXT
    } > FLASH

    KICKOS_CORTEXM_EH
    KICKOS_CORTEXM_CTORS
    KICKOS_CORTEXM_EXIDX
    KICKOS_INIT_TABLES(FLASH)
    KICKOS_CORTEXM_DATA(KICKOS_APPDATA_SECTION(_ebss))

    /* The FlexSPI XIP window as the app's RX region. */
    __kickos_code_start = ORIGIN(FLASH);
    __kickos_code_end = ORIGIN(FLASH) + LENGTH(FLASH);

    /* Boot Data.length: the on-flash image extent, from flash base to the end of the loaded
       .data (highest LMA). */
    __boot_image_length = LOADADDR(.data) + SIZEOF(.data) - ORIGIN(FLASH);

    KICKOS_CORTEXM_DISCARD
}

KICKOS_CORTEXM_ASSERTS()
KICKOS_KERNEL_CTORS_NONE_ASSERT()

#include <boot_arena.ld.h>
KICKOS_BOOT_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)
KICKOS_POOL_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)
