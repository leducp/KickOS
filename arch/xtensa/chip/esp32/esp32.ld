/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the ESP32-D0WDQ6 (WROOM-32), Xtensa LX6. The image links
 * ENTIRELY into internal SRAM (no flash cache / XIP), so the ROM loader copies
 * every segment to its RAM load address and jumps -- no second stage, no MMU
 * setup (see startup.S). Addresses are from the ESP32 TRM
 * 1.3.2 "Embedded Memory" (Table 1-3, internal SRAM address mapping):
 *
 *   Internal SRAM0 (IRAM, exec): 0x4007_0000 .. 0x4009_FFFF (192 KiB; low 64 KiB
 *                                is the cache region) -- code links at 0x4008_0000.
 *   Internal SRAM2 (DRAM, data): 0x3FFA_E000 .. 0x3FFD_FFFF (200 KiB) -- data links
 *                                at 0x3FFB_0000.
 *
 * IRAM is word-access only, so .rodata (byte-addressable strings/const data) lives
 * in DRAM, not with .text.
 *
 * Windowed ABI: the mandatory WindowOverflow/Underflow vectors are part of the
 * .vectors table placed at VECBASE (startup.S); they spill/fill to each thread's
 * OWN stack, so no dedicated exception stack is reserved here.
 */

ENTRY(_start)

MEMORY
{
    IRAM (rx)  : ORIGIN = 0x40080000, LENGTH = 128K
    DRAM (rw)  : ORIGIN = 0x3FFB0000, LENGTH = 192K
}

_estack = ORIGIN(DRAM) + LENGTH(DRAM); /* initial stack: top of DRAM */
_kernel_stack_size = 8K;               /* boot/interrupt stack reserve */

SECTIONS
{
    /* Xtensa exception/interrupt vectors at VECBASE. Must be 1 KiB aligned; the
       startup sets wsr.vecbase to _kickos_vectors. Placed at IRAM origin. */
    .vectors ORIGIN(IRAM) :
    {
        _kickos_vectors = .;
        KEEP(*(.vectors))
        . = ALIGN(4);
    } > IRAM

    .text : ALIGN(4)
    {
        *(.literal .literal.*)
        *(.text .text.*)
        . = ALIGN(4);
    } > IRAM

    .rodata : ALIGN(4)
    {
        *(.rodata .rodata.*)
        *(.rodata_wlog .rodata_wlog.*)
        . = ALIGN(4);
    } > DRAM

    .init_array : ALIGN(4)
    {
        __init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    } > DRAM

    _sidata = LOADADDR(.data);

    .data : ALIGN(4)
    {
        _sdata = .;
        *(.data .data.*)
        . = ALIGN(4);
        _edata = .;
    } > DRAM AT > DRAM

    .bss (NOLOAD) : ALIGN(4)
    {
        _sbss = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > DRAM

    /* User-RAM pool (arch_ram_alloc) between static data and the reserved stack. */
    . = ALIGN(16);
    __kickos_ram_start = .;
    __kickos_ram_end = _estack - _kernel_stack_size;

    /DISCARD/ : { *(.xtensa.info) *(.comment) *(.xt.*) }
}

ASSERT(__kickos_ram_start <= __kickos_ram_end,
       "KickOS: static data overflows DRAM (no room for the user-RAM pool + stack)")
