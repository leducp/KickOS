/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the NXP MK64FN1M0VLL12 (FRDM-K64F), Cortex-M4F.
 * (K64 Sub-Family Reference Manual K64P144M120SF5RM.)
 *
 *   Flash : 1 MiB  @ 0x0000_0000
 *   SRAM  : 256 KiB, contiguous: SRAM_L 64 KiB @ 0x1FFF_0000 + SRAM_U 192 KiB
 *           @ 0x2000_0000  ->  0x1FFF_0000 .. 0x2003_0000
 *
 * CRITICAL: the 16-byte Flash Configuration Field at 0x0000_0400 (FSEC/FOPT/
 * FPROT) is loaded at reset. A wrong/zeroed FSEC secures or mass-erase-disables
 * the part (bricks debug access), so .fcf is placed here explicitly and KEPT.
 */

ENTRY(Reset_Handler)

/* SRAM is two banks joined at 0x2000_0000 (SRAM_L below, SRAM_U above). Aligned
 * CPU word accesses cross it fine; unaligned/burst accesses across the seam are
 * unsupported (RM 3.5.3.1) -- see -mno-unaligned-access in the toolchain, and
 * split this region if a DMA/burst buffer ever needs to straddle 0x2000_0000. */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 1024K
    RAM   (rwx) : ORIGIN = 0x1FFF0000, LENGTH = 256K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);   /* initial MSP: top of SRAM_U */
_kernel_stack_size = 8K;               /* boot/handler (MSP) stack */

SECTIONS
{
    .isr_vector ORIGIN(FLASH) :
    {
        KEEP(*(.isr_vector))
    } > FLASH

    /* Flash Configuration Field at a fixed 0x400; the vector table (102 words =
       0x198) fits below it, code follows above 0x410. */
    .fcf 0x00000400 :
    {
        KEEP(*(.fcf))
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
