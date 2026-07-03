/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the Raspberry Pi RP2040 (Pico), Cortex-M0+. Code executes in
 * place (XIP) from external QSPI flash at 0x1000_0000; the first 256 bytes are
 * the second-stage bootloader (boot2, .boot2 section, CRC-checked by the ROM),
 * and the vector table follows at 0x1000_0100. SRAM is 264 KiB @ 0x2000_0000.
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x10000000, LENGTH = 2048K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 264K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
_kernel_stack_size = 8K;

SECTIONS
{
    /* boot2: exactly 256 bytes at flash start (the ROM loads + CRC-checks it). */
    .boot2 ORIGIN(FLASH) :
    {
        KEEP(*(.boot2))
    } > FLASH

    .text :
    {
        KEEP(*(.isr_vector))   /* vector table at 0x1000_0100 */
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.glue_7) *(.glue_7t)
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
    } > FLASH

    .init_array :
    {
        __init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    } > FLASH

    .ARM.exidx :
    {
        __exidx_start = .;
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        __exidx_end = .;
    } > FLASH

    _sidata = LOADADDR(.data);

    .data : ALIGN(4)
    {
        _sdata = .;
        *(.data .data.*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss (NOLOAD) : ALIGN(4)
    {
        _sbss = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    . = ALIGN(32);
    __kickos_ram_start = .;
    __kickos_ram_end = _estack - _kernel_stack_size;

    /DISCARD/ : { *(.ARM.attributes) }
}

ASSERT(__kickos_ram_start <= __kickos_ram_end,
       "KickOS: static data overflows RAM (no room for the user-RAM pool + stack)")

/* The vector table must sit immediately after the 256-byte boot2. */
ASSERT(ADDR(.text) == ORIGIN(FLASH) + 0x100,
       "KickOS: RP2040 vector table must be at 0x1000_0100 (after boot2)")
