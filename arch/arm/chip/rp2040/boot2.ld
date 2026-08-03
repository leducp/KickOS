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
    } > RAM

    /DISCARD/ : { *(.ARM.attributes) *(.comment) *(.note*) }
}
