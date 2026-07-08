/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the QEMU `virt` RISC-V board (RV32IMAC, M-mode bare metal) --
 * the runnable rv32imac verification target. QEMU (`-bios none -kernel <elf>`)
 * loads the image into DRAM at 0x8000_0000 and enters it in machine mode, so the
 * whole image links to run from RAM (LMA == VMA; the Reset_Handler .data copy is a
 * no-op). _start (.text.init) must be first at the RAM origin.
 *
 * Chip<->arch contract: define __kickos_ram_start/__kickos_ram_end (the
 * arch_ram_alloc pool), _estack (boot stack top), and _sidata/.../_ebss (C
 * runtime). __global_pointer$ anchors gp-relative small-data access.
 */

ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 8M   /* QEMU virt DRAM (ample) */
}

_estack = ORIGIN(RAM) + LENGTH(RAM);   /* boot/handler stack: top of RAM */
_kernel_stack_size = 16K;              /* boot/trap stack reservation */

SECTIONS
{
    .text ORIGIN(RAM) : ALIGN(4)
    {
        KEEP(*(.text.init))            /* _start -- must be at the RAM origin */
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

    _sidata = LOADADDR(.data);          /* == VMA here (RAM image), so the copy is a no-op */

    .data : ALIGN(8)
    {
        _sdata = .;
        *(.data .data.*)
        . = ALIGN(8);
        PROVIDE(__global_pointer$ = . + 0x800);  /* gp: middle of the small-data window */
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

    /* From here up to _kernel_stack_size below _estack is the arch_ram_alloc pool
       (runtime bump-allocated -- just symbols, no section to reserve). */
    . = ALIGN(16);
    __kickos_ram_start = .;
    __kickos_ram_end = _estack - _kernel_stack_size;

    /DISCARD/ : { *(.comment) *(.riscv.attributes) *(.eh_frame) }
}

ASSERT(__kickos_ram_start <= __kickos_ram_end,
       "KickOS: static data overflows RAM (no room for the user-RAM pool + boot stack)")
