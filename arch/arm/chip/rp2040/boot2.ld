/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Link script for the RP2040 second stage (boot2.S) ONLY -- not the application.
 * boot2 is position-independent and runs from an SRAM address chosen by the
 * bootrom, so ORIGIN is nominal; LENGTH caps the payload at 252 bytes (the ROM's
 * CRC-32 occupies bytes 252..255, appended later by rp2040_checksum.py).
 *
 * A driver-level -T (this file) is REQUIRED: the picolibc-based arm-none-eabi
 * toolchain injects its own default linker script unless it sees a -T at the gcc
 * driver level, and that default's tiny `flash` region collides with boot2.
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
    } > RAM

    /DISCARD/ : { *(.ARM.attributes) *(.comment) *(.note*) }
}
