/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Link script for the RP2040 second stage (boot2.S) ONLY, not the application.
 * boot2 is position-independent and runs from an SRAM address chosen by the
 * bootrom, so ORIGIN is nominal; LENGTH caps the payload at 252 bytes (the ROM's
 * CRC-32 occupies bytes 252..255, appended later by rp2040_checksum.py).
 *
 * This file is supplied via a driver-level -T. The newlib arm-none-eabi
 * toolchain injects no default linker script, so this script applies alone and
 * caps the payload as intended. A toolchain that injects a default script unless
 * it sees a driver-level -T needs the flag there, not at the link step.
 */

MEMORY
{
    RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 252
}

ENTRY(_rp2040_boot2_entry)

SECTIONS
{
    .text : {
        KEEP(*(.text))
        *(.text*)
        *(.rodata*)
        *(.glue_7) *(.glue_7t) *(.vfp11_veneer) *(.v4_bx)
    } > RAM

    /* boot2 is one .S with no data of its own, and the bootrom copies the payload with no
       .data relocation and no .bss zeroing. Named so --orphan-handling=error has a rule and
       ASSERTed empty below, because a byte here would be loaded but never initialised. This
       script is not preprocessed, so it cannot share arch/common/sections.ld.h. */
    .boot2_nodata (NOLOAD) : {
        __boot2_nodata_start = .;
        *(.data*) *(.bss*) *(COMMON)
        __boot2_nodata_end = .;
    } > RAM

    /DISCARD/ : { *(.ARM.attributes) *(.comment) *(.note*) }
}

ASSERT(__boot2_nodata_end == __boot2_nodata_start,
       "KickOS: the RP2040 second stage grew a .data or .bss object; nothing initialises it, the bootrom copying the 252-byte payload verbatim")
