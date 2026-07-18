/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the STM32F302R8 (Nucleo-F302R8), Cortex-M4F.
 *   Flash : 64 KiB @ 0x0800_0000
 *   SRAM  : 16 KiB @ 0x2000_0000   (tiny -- provisioning is sized down to match)
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 64K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 16K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
_kernel_stack_size = 2K;

SECTIONS
{
    .text :
    {
        KEEP(*(.isr_vector))
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.glue_7) *(.glue_7t)
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
    } > FLASH

    /* Global ctors SPLIT: .init_array keeps ONLY the closed KickOS-owned set
       (kernel/arch/chip/lib), run early by Reset_Handler; other (app/lib) ctors
       land in .kickos_app_init_array and run from root_entry with the kernel live. */
    .init_array :
    {
        __init_array_start = .;
        KEEP(*libkickos_kernel.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_arch_armv7m.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_chip_stm32f302.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_lib.a:*(SORT(.init_array.*) .init_array))
        __init_array_end = .;
    } > FLASH

    .kickos_app_init_array :
    {
        __kickos_app_init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __kickos_app_init_array_end = .;
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
