/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the STM32F103, Cortex-M3. This is the CHIP DEFAULT, sized to
 * the LOW-DENSITY floor (10 KiB SRAM) because "Blue Pill" boards are very often
 * low-density parts/clones (32 KiB flash / 10 KiB SRAM, DEV_ID 0x412). At 20 KiB
 * the initial SP (_estack = top of RAM) lands past a 10 KiB die -> the first push
 * hardfaults before main. 10 KiB runs on both variants (a real C8 just wastes the
 * upper half). A board that KNOWS it has the full 20 KiB C8 overrides this with
 * its own linker script (see boards/bluepill-c8/stm32f103.ld).
 *   Flash : 32 KiB @ 0x0800_0000  (aliased to 0x0 at boot; vectors live here)
 *   SRAM  : 10 KiB @ 0x2000_0000
 * (both are the low-density floor: a genuine C8 has 64 KiB/20 KiB and overrides
 *  via boards/bluepill-c8/stm32f103.ld -- so this catches a >32 KiB image at
 *  link time rather than letting it fail to fit an LD clone at flash time.)
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 32K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 10K
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
