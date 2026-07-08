/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the ESP32-C6-WROOM-1 (ESP-RISC-V, RV32IMAC). BUILD-ONLY: the
 * image links to run from the unified HP SRAM (the ROM loader copies segments in,
 * so LMA == VMA and the Reset_Handler .data copy is a no-op) -- the classic-ESP32
 * posture. A bootable image (esptool elf2image) + the exact SRAM sub-regions are
 * HW-pass work.
 *
 * ESP32-C6 (C6 TRM memory map): 512 KiB unified HP SRAM at 0x4080_0000. _start
 * (.text.init) is placed first. Chip<->arch contract: __kickos_ram_start/_end (the
 * arch_ram_alloc pool), _estack, and the C-runtime _sidata/.../_ebss symbols.
 */

ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x40800000, LENGTH = 512K   /* HP SRAM */
}

_estack = ORIGIN(RAM) + LENGTH(RAM);   /* boot/handler stack: top of HP SRAM */
_kernel_stack_size = 16K;

SECTIONS
{
    .text ORIGIN(RAM) : ALIGN(4)
    {
        KEEP(*(.text.init))            /* _start first */
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
        . = ALIGN(4);
    } > RAM

    .init_array : ALIGN(4)
    {
        __init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    } > RAM

    _sidata = LOADADDR(.data);

    .data : ALIGN(8)
    {
        _sdata = .;
        *(.data .data.*)
        . = ALIGN(8);
        PROVIDE(__global_pointer$ = . + 0x800);
        *(.sdata .sdata.* .sdata2.*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > RAM

    .bss (NOLOAD) : ALIGN(4)
    {
        _sbss = .;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    . = ALIGN(16);
    __kickos_ram_start = .;
    __kickos_ram_end = _estack - _kernel_stack_size;

    /DISCARD/ : { *(.comment) *(.riscv.attributes) *(.eh_frame) }
}

ASSERT(__kickos_ram_start <= __kickos_ram_end,
       "KickOS: static data overflows HP SRAM (no room for the user-RAM pool + boot stack)")
