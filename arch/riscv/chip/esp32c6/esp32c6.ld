/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the ESP32-C6-WROOM-1 (ESP-RISC-V, RV32IMAC). The image links
 * to run from the unified HP SRAM (the ROM loader copies segments in, so LMA == VMA
 * and the Reset_Handler .data copy is a no-op) -- the classic-ESP32 posture. A
 * bootable image (esptool elf2image) + the exact SRAM sub-regions are HW-pass work.
 *
 * ESP32-C6 (C6 TRM Table 1.4-1): the HP SRAM sits in the single unified
 * Instruction/Data region 0x4000_0000..0x4FFF_FFFF (R/W, one address for both fetch
 * and load/store -- unlike the classic ESP32's split IRAM/DRAM windows). So ONE pow2
 * NAPOT code region covers unprivileged instruction fetch AND the shared-RX read.
 * 512 KiB unified HP SRAM at 0x4080_0000. _start (.text.init) is placed first.
 * Chip<->arch contract: __kickos_ram_start/_end (the arch_ram_alloc pool), _estack,
 * and the C-runtime _sidata/.../_ebss symbols.
 *
 * Under KICKOS_HAVE_MPU the script also carves the two linker-defined regions every
 * UNPRIVILEGED thread needs under PMP enforcement (see arch_domain_static_regions),
 * exactly as the qemu-riscv virt.ld does (shared rv32imac PMP backend):
 *   __kickos_code_start/_end     -- all code+rodata as ONE pow2 NAPOT region (RX)
 *   __kickos_appdata_start/_end  -- app .data/.bss as ONE pow2 NAPOT region (RW-NX)
 * All-SRAM target: the code region is carved from the SRAM image at ORIGIN, which is
 * 8 MiB-aligned (0x4080_0000), so any pow2 sub-block up to 8 MiB is naturally
 * NAPOT-aligned. App globals reach .appdata only when user objects are built with
 * -msmall-data-limit=0 (routes them to .data/.bss, not the gp-relative .sdata/.sbss
 * window that lives with the kernel small-data around __global_pointer$).
 */

ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x40800000, LENGTH = 512K   /* HP SRAM */
}

_estack = ORIGIN(RAM) + LENGTH(RAM);   /* boot/handler stack: top of HP SRAM */
_kernel_stack_size = 16K;

/* Pow2 sizes for the two unprivileged NAPOT regions. The code block starts at ORIGIN
   (8 MiB-aligned) and the app-data block at __kickos_code_end (= ORIGIN + _code_size,
   still pow2-aligned), so each region base is naturally aligned to its size. Raise
   _code_size / _appdata_size to the next pow2 if an ASSERT below trips. */
#if KICKOS_HAVE_MPU
_code_size = 64K;
_appdata_size = 4K;
#endif

SECTIONS
{
    .text ORIGIN(RAM) : ALIGN(4)
    {
#if KICKOS_HAVE_MPU
        __kickos_code_start = .;       /* == ORIGIN(RAM): the code NAPOT region base */
#endif
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
#if KICKOS_HAVE_MPU
        . = ALIGN(4);
        _code_used_end = .;            /* end of real code+rodata (checked below) */
#endif
    } > RAM

#if KICKOS_HAVE_MPU
    /* The code NAPOT region spans [ORIGIN, ORIGIN + _code_size). The end is a symbol
       (not a section pad) so the image carries no zero fill. The gap between
       _code_used_end and __kickos_code_end is unused SRAM covered by the RX grant --
       harmless (an unprivileged thread never branches there). */
    __kickos_code_end = __kickos_code_start + _code_size;

    /* App-owned globals as a pow2 NAPOT region, pinned by EXPLICIT VMA right after the
       code region. Grouped by path (objects under a user/ dir = the app object + user
       runtime), placed BEFORE the kernel .data/.bss so those input sections are
       consumed here and the kernel catch-all gets only kernel/arch/lib. Both sub-
       sections carry an explicit address: an explicit VMA on an EMPTY output section
       does NOT advance the `> RAM` region pointer, so deriving .appbss's start from
       SIZEOF(.appdata) pins it regardless. .appdata is loaded in place (LMA == VMA on
       this RAM image); .appbss + the pow2 pad are zeroed by the Reset_Handler (no
       stale read-back through the RW grant). App globals land here only under
       -msmall-data-limit=0 (see the header note). */
    .appdata __kickos_code_end : ALIGN(4)
    {
        __kickos_appdata_start = .;
        *user*(.data .data.*)
        . = ALIGN(4);
    } > RAM AT > RAM

    .appbss (__kickos_appdata_start + SIZEOF(.appdata)) (NOLOAD) : ALIGN(4)
    {
        __kickos_appbss_start = .;
        *user*(.bss .bss.*)
        *user*(COMMON)
        . = ALIGN(4);
        _appdata_used_end = .;
        . = __kickos_appdata_start + _appdata_size; /* pad the block up to pow2 */
        __kickos_appdata_end = .;
    } > RAM
#endif

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

#if KICKOS_HAVE_MPU
/* Real code+rodata must fit the pow2 code region (raise _code_size to the next pow2
   if this trips). */
ASSERT(_code_used_end <= __kickos_code_end,
       "KickOS: app code overflows _code_size (raise it to the next pow2)")
/* App globals must fit the pow2 app-data region. A zero-sized used-end means the
   user-path selector matched nothing -- check the build layout / small-data flag. */
ASSERT(_appdata_used_end <= __kickos_appdata_start + _appdata_size,
       "KickOS: app .data/.bss overflow _appdata_size (raise it to the next pow2)")
#endif
