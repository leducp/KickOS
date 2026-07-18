/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the QEMU `mps2-an386` (ARM MPS2 FPGA, Cortex-M4F) -- the
 * runnable armv7m CI/verification target. QEMU maps a code region at
 * 0x00000000 (vectors read at reset, VTOR=0) and SRAM at 0x20000000.
 *
 * Chip<->arch contract: define __kickos_ram_start/__kickos_ram_end (the
 * arch_ram_alloc pool) and _estack (initial MSP = vector[0]).
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 4M
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 4M
}

_estack = ORIGIN(RAM) + LENGTH(RAM);   /* initial MSP: top of RAM */
_kernel_stack_size = 16K;              /* boot/handler (MSP) stack */

SECTIONS
{
    .isr_vector ORIGIN(FLASH) :
    {
        KEEP(*(.isr_vector))
    } > FLASH

    .text :
    {
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
        KEEP(*libkickos_chip_mps2.a:*(SORT(.init_array.*) .init_array))
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

    _sidata = LOADADDR(.data);           /* .data init image lives in flash */

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

    /* Everything from here up to _kernel_stack_size below _estack is the
       arch_ram_alloc pool (runtime bump-allocated, so just symbols -- no section
       to reserve). The top _kernel_stack_size bytes are the MSP stack. */
    . = ALIGN(32);
    __kickos_ram_start = .;
    __kickos_ram_end = _estack - _kernel_stack_size;

    /DISCARD/ : { *(.ARM.attributes) }
}

/* Static data (.data+.bss) must not eat into the user-RAM pool / MSP stack.
   Link-time guard: arch_ram_size() = end - start; a negative diff would wrap to
   a huge pool. Critical on small-SRAM parts (this .ld is the item-10 template). */
ASSERT(__kickos_ram_start <= __kickos_ram_end,
       "KickOS: static data overflows RAM (no room for the user-RAM pool + stack)")
