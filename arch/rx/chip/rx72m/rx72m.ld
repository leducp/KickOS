/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the Renesas RX72M (RX72M Group, RXv3 core).
 * (RX72M Group User's Manual: Hardware, r01uh0804ej0120, Rev.1.20.)
 *
 *   Code flash : 4 MiB  @ 0xFFC0_0000 .. 0xFFFF_FFFF (UM memory map)
 *   SRAM       : 512 KiB @ 0x0000_0000 (main on-chip RAM; expansion RAM omitted)
 *   OFSM       : option-setting memory @ 0xFE7F_5D00 (MDE/OFS0/OFS1) -- data in
 *                the image, not code (UM sec.7). Determines endian + WDT/LVD state.
 *
 * Two vector tables (UM sec.2.6):
 *   - Fixed exception table at 0xFFFF_FF80 (EXTB reset value). Its last word,
 *     0xFFFF_FFFC, is the RESET vector (fetched by hardware at power-on).
 *   - Relocatable interrupt table (INTB), 256 words in ROM; startup sets INTB.
 *
 * RX section names: GCC emits .text/.rodata/.data/.bss for KickOS TUs, but the
 * GNU RX libc/libgcc use the RX-native short names P (code), C/C_1/C_2/W/L
 * (const), D/D_1/D_2 (data), B/B_1/B_2 (bss) -- all matched below.
 *
 * RX symbol names: the RX psABI user-label-prefix is '_', so a C `extern _sidata`
 * resolves to `__sidata` and `extern __kickos_ram_start` to `___kickos_ram_start`.
 * The link-time symbols below carry that extra leading underscore to match; asm
 * symbols (e.g. _istack) are used verbatim and take no extra prefix.
 */

ENTRY(_start)

MEMORY
{
    RAM   (rwx) : ORIGIN = 0x00000000, LENGTH = 512K
    OFSM  (r)   : ORIGIN = 0xFE7F5D00, LENGTH = 0x10
    ROM   (rx)  : ORIGIN = 0xFFC00000, LENGTH = 0x003FFF80  /* up to 0xFFFFFF80 */
    FVECT (rx)  : ORIGIN = 0xFFFFFF80, LENGTH = 0x80        /* fixed vectors + reset */
}

_istack = ORIGIN(RAM) + LENGTH(RAM);   /* boot/interrupt stack (ISP): top of SRAM */
_kernel_stack_size = 8K;               /* ISP (boot/ISR) stack reservation */

SECTIONS
{
    /* Option-setting memory: little-endian + WDT/LVD off (all-ones defaults;
       UM sec.7.2.5 MDE little-endian = 0xFFFFFFFF, sec.7.2.3/4 OFS0/OFS1). */
    .ofsm : { KEEP(*(.ofsm)) } > OFSM

    .rvectors : ALIGN(4)
    {
        KEEP(*(.rvectors))     /* 256-word relocatable interrupt table (INTB) */
    } > ROM

    .text : ALIGN(4)
    {
        *(.text .text.*)
        *(P P_*)               /* RX-native code (libc/libgcc) */
        *(.rodata .rodata.*)
        *(C C_1 C_2 C$*)       /* RX-native const */
        *(W W_1 W_2)           /* RX-native const (16/8-bit addressed) */
        *(L)                   /* literal pool */
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
    } > ROM

    .init_array : ALIGN(4)
    {
        ___init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        ___init_array_end = .;
    } > ROM

    __sidata = LOADADDR(.data);

    .data : ALIGN(4)
    {
        __sdata = .;
        *(.data .data.*)
        *(D D_1 D_2)           /* RX-native initialized data */
        . = ALIGN(4);
        __edata = .;
    } > RAM AT > ROM

    .bss (NOLOAD) : ALIGN(4)
    {
        __sbss = .;
        *(.bss .bss.*)
        *(B B_1 B_2)           /* RX-native bss */
        *(COMMON)
        . = ALIGN(4);
        __ebss = .;
    } > RAM

    . = ALIGN(32);
    ___kickos_ram_start = .;
    ___kickos_ram_end = _istack - _kernel_stack_size;

    /* Fixed exception vector table incl the reset vector at 0xFFFFFFFC. */
    .fvectors : { KEEP(*(.fvectors)) } > FVECT

    /DISCARD/ : { *(.ARM.attributes) *(.comment) }
}

ASSERT(___kickos_ram_start <= ___kickos_ram_end,
       "KickOS: static data overflows RAM (no room for the user-RAM pool + ISP stack)")
