/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * NOT a C header: the GNU ld body every RV32IMAC chip script includes after its MEMORY, which
 * names RAM (and SHARED for an own-image AMP node). The whole image runs from RAM, _start
 * (.text.init) first at its origin.
 *
 * KICKOS_RV32_LOADS_AT_VMA, which the chip defines, is 1 where the loader places every segment
 * at its VMA (an esptool image): no section carries an AT clause, the chip's Reset_Handler
 * copies from _sidata and _appdata_lma, and LMA == VMA is asserted. At 0 the loader places each
 * segment at its PhysAddr, .data's VMA is pinned past the code pad with its bytes at the LMA,
 * and kickos_ranges_init moves them by the copy table.
 *
 * Under KICKOS_HAVE_MPU the script carves the two regions every UNPRIVILEGED thread needs under
 * PMP enforcement (arch_domain_static_regions):
 *   __kickos_code_start/_end     code+rodata as ONE pow2 NAPOT region (RX)
 *   __kickos_appdata_start/_end  app .data/.bss as ONE pow2 NAPOT region (RW-NX)
 * The code block starts at ORIGIN(RAM) and the app-data block at __kickos_code_end +
 * _kernel_data_reserve, both multiples of _appdata_size, so each base is aligned to its size
 * as long as the RAM origin is aligned to the code block.
 *
 * The single gp small-data window (__global_pointer$ +/- 0x800) is anchored INSIDE .appdata:
 * the KickOS libs are built -msmall-data-limit=0, so the window holds only app and C++ runtime
 * small data and the app grant covers it.
 */
#ifndef KICKOS_ARCH_RISCV_COMMON_RV32IMAC_LD_H
#define KICKOS_ARCH_RISCV_COMMON_RV32IMAC_LD_H

#include <sections.ld.h>
#include <kernel_data_reserve.ld.h>

#ifndef KICKOS_RV32_LOADS_AT_VMA
#error "KickOS: define KICKOS_RV32_LOADS_AT_VMA (0 or 1) in the chip script before including rv32imac.ld.h"
#endif
#if KICKOS_RV32_LOADS_AT_VMA
#define KICKOS_RV32_LOAD
#else
#define KICKOS_RV32_LOAD AT > RAM
#endif

ENTRY(_start)

_estack = ORIGIN(RAM) + LENGTH(RAM);
_kernel_stack_size = 16K;

/* Both NAPOT sizes are powers of two; raise one to the next if an ASSERT below trips. */
#if KICKOS_HAVE_MPU
_code_size = KICKOS_CODE_SIZE;
/* The app window's pad is the newlib heap: a 32K window leaves about 14K, room for the
   cxxtest peak (about 4.2K on RISC-V DWARF) and printf. */
#ifndef KICKOS_APPDATA_SIZE
#define KICKOS_APPDATA_SIZE 32K
#endif
_appdata_size = KICKOS_APPDATA_SIZE;
/* Two 32K windows: the kernel's .data/.bss runs a few hundred bytes past one. */
#ifndef KICKOS_KERNEL_DATA_RESERVE
#define KICKOS_KERNEL_DATA_RESERVE 64K
#endif
#endif

SECTIONS
{
    .text ORIGIN(RAM) : ALIGN(4)
    {
#if KICKOS_HAVE_MPU
        __kickos_code_start = .;
#endif
        KEEP(*(.text.init))
        *(.text .text.*)
        KICKOS_CODE_DEBRIS_SECTIONS
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
        /* DWARF EH, registered by the root thread (user/src/root_entry.cc): -nostartfiles
           drops crtbegin's frame_dummy and crtend's __FRAME_END__, so the table is kept whole
           and ends in the zero-length CIE crtend would supply. */
        *(.gcc_except_table .gcc_except_table.*)
        . = ALIGN(4);
        __eh_frame_start = .;
        KEEP(*(.eh_frame))
        LONG(0)
        . = ALIGN(4);
    } > RAM

    /* .init_array holds ONLY the closed KickOS-owned set, run by Reset_Handler; every other
       ctor runs from root_entry with the kernel live. Both are code, so _code_used_end trails
       the app's (end of all code). */
    .init_array : ALIGN(4)
    {
        __init_array_start = .;
        KEEP(*libkickos_kernel.a:*(SORT(.init_array.*) .init_array))
        KEEP(*KICKOS_LD_ARCH_LIB:*(SORT(.init_array.*) .init_array))
        KEEP(*KICKOS_LD_CHIP_LIB:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_lib.a:*(SORT(.init_array.*) .init_array))
        __init_array_end = .;
    } > RAM

#if !KICKOS_RV32_LOADS_AT_VMA
    KICKOS_INIT_TABLES(RAM)
#endif

    /* Asserted empty; nothing walks it. */
    .kickos_app_fini_array : ALIGN(4)
    {
        __kickos_app_fini_array_start = .;
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array))
        __kickos_app_fini_array_end = .;
    } > RAM

    KICKOS_TLS_TEMPLATE(RAM)

    .kickos_app_init_array : ALIGN(4)
    {
        __kickos_app_init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __kickos_app_init_array_end = .;
#if KICKOS_HAVE_MPU
        . = ALIGN(4);
        _code_used_end = .;
#endif
    } > RAM

#if KICKOS_HAVE_MPU
    /* A symbol and not a section pad, so the image carries no zero fill; the gap past
       _code_used_end is unused SRAM inside the RX grant. */
    __kickos_code_end = __kickos_code_start + _code_size;
    KICKOS_KERNEL_DATA_RESERVE_DECL(__kickos_code_end)

#if KICKOS_RV32_LOADS_AT_VMA
    _sidata = LOADADDR(.data);
#endif

    /* THE SELECTION IS INVERTED: the closed KickOS-owned set is captured FIRST by
       `archive:member` selectors into the kernel .data/.bss right after the code region, and
       the .appdata/.appbss catch-alls take everything else, gp small data included. Kernel
       .data is pinned at __kickos_code_end and the app window at _kernel_data_top, so the
       window, the heap and the arena sit at declared addresses rather than following kernel
       .bss over its alignment cliff.

       Small data from a KickOS lib is captured here, ahead of the catch-alls, and refused at
       the end of this file. */
    .kickos_kernel_sdata :
    {
        *libkickos_kernel.a:*(.sdata* .sbss*)
        *KICKOS_LD_ARCH_LIB:*(.sdata* .sbss*)
        *KICKOS_LD_CHIP_LIB:*(.sdata* .sbss*)
        *libkickos_lib.a:*(.sdata* .sbss*)
        *kickos_string.dir*(.sdata* .sbss*)
    }

    .data __kickos_code_end : ALIGN(8)
    {
        _sdata = .;
        *libkickos_kernel.a:*(.data .data.*)
        *KICKOS_LD_ARCH_LIB:*(.data .data.*)
        *KICKOS_LD_CHIP_LIB:*(.data .data.*)
        *libkickos_lib.a:*(.data .data.*)
        . = ALIGN(4);
        _edata = .;
    } > RAM KICKOS_RV32_LOAD

    .bss (NOLOAD) : ALIGN(4)
    {
        _sbss = .;
        __kernel_bss_start = .;
        *libkickos_kernel.a:*(.bss .bss.*)
        *KICKOS_LD_ARCH_LIB:*(.bss .bss.*)
        *KICKOS_LD_CHIP_LIB:*(.bss .bss.*)
        *libkickos_lib.a:*(.bss .bss.*)
        *libkickos_kernel.a:*(COMMON)
        *KICKOS_LD_ARCH_LIB:*(COMMON)
        *KICKOS_LD_CHIP_LIB:*(COMMON)
        *libkickos_lib.a:*(COMMON)
        __kernel_bss_end = .;
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    /* .appbss is pinned right after .appdata via SIZEOF, so the zero of
       [end of the loaded bytes, __kickos_appdata_end) covers the whole grant with no gap. */
    KICKOS_APPDATA_SECTION(_ebss)
    {
        __kickos_appdata_start = .;
        *(.data .data.*)
        . = ALIGN(8);
        __sdata_start = .;
        PROVIDE(__global_pointer$ = . + 0x800);
        *(.sdata .sdata.* .sdata2.*)
        . = ALIGN(8);
        __sdata_end = .;
#if !KICKOS_RV32_LOADS_AT_VMA
        __kickos_appdata_load_end = .;
#endif
    } > RAM KICKOS_RV32_LOAD
#if KICKOS_RV32_LOADS_AT_VMA
    _appdata_lma = LOADADDR(.appdata);
#endif

    .appbss (__kickos_appdata_start + SIZEOF(.appdata)) (NOLOAD) : ALIGN(4)
    {
        __kickos_appbss_start = .;
        *(.sbss .sbss.*)
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        _appdata_used_end = .;
        /* MAX: ld evaluates ASSERTs after layout, and a pad moving backwards would fail the
           link ahead of the overflow assert. */
        . = MAX(., __kickos_appdata_start + _appdata_size);
        __kickos_appdata_end = .;
        /* The newlib heap IS the window pad, its base aligned relative to this section
           (app_window.ld.h). */
        _kickos_heap_start = ALIGN(_appdata_used_end, 8);
        _kickos_heap_limit = __kickos_appdata_end;
    } > RAM
#else
#if KICKOS_RV32_LOADS_AT_VMA
    _sidata = LOADADDR(.data);
#endif

    .data : ALIGN(8)
    {
        _sdata = .;
        *(.data .data.*)
        . = ALIGN(8);
        PROVIDE(__global_pointer$ = . + 0x800);
        *(.sdata .sdata.* .sdata2.*)
        . = ALIGN(4);
        _edata = .;
    } > RAM KICKOS_RV32_LOAD

    .bss (NOLOAD) : ALIGN(4)
    {
        _sbss = .;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    .userheap (NOLOAD) :
    {
        . = ALIGN(8);
        _kickos_heap_start = .;
        . += KICKOS_USER_HEAP_SIZE;
        _kickos_heap_limit = .;
    } > RAM
#endif

    . = ALIGN(16);
    __kickos_ram_start = .;
    __kickos_ram_end = _estack - _kernel_stack_size;

    __kickos_rom_start = 0;
    __kickos_rom_end = 0;
    __kickos_sram_start = ORIGIN(RAM);

#if KICKOS_AMP_OWN_IMAGE
    KICKOS_AMP_SHARED_SECTION(SHARED)
#endif

    /DISCARD/ : { *(.comment) *(.riscv.attributes) }

    KICKOS_NONALLOC_SECTIONS
}

KICKOS_STATIC_RELOC_ASSERT()
KICKOS_TLS_ALIGN_ASSERT()
KICKOS_TLS_FIT_ASSERT()

ASSERT(__kickos_ram_start <= __kickos_ram_end,
       "KickOS: static data overflows RAM (no room for the user-RAM pool + boot stack)")

KICKOS_FINI_EMPTY_ASSERT()

#if KICKOS_RV32_LOADS_AT_VMA
/* An LMA the loader never writes would have Reset_Handler's copies read uninitialized SRAM
   over the loaded data. */
ASSERT(_sidata == _sdata,
       "KickOS: .data LMA != VMA: the ROM loader places segments at their VMAs, so the LMA holds no image bytes (drop the AT clause)")
#endif

#if KICKOS_HAVE_MPU
ASSERT(_code_used_end <= __kickos_code_end,
       "KickOS: app code overflows _code_size (raise it to the next pow2)")
ASSERT(_appdata_used_end <= __kickos_appdata_start + _appdata_size,
       "KickOS: app .data/.bss overflow _appdata_size (raise it to the next pow2)")
/* An empty kernel .bss means the closed-set selectors matched nothing, kernel writable state
   having leaked into the app catch-all. */
ASSERT(__kernel_bss_end > __kernel_bss_start,
       "KickOS: kernel .bss empty: closed-set archive selector matched nothing (renamed lib?)")
/* Past the psABI's gp reach, gp-relative loads resolve at the wrong addresses. */
ASSERT(__sdata_end <= __global_pointer$ + 0x800,
       "KickOS: runtime small-data overflows the gp + 0x800 reach")
ASSERT(__sdata_start >= __global_pointer$ - 0x800,
       "KickOS: runtime small-data underflows the gp - 0x800 reach")
ASSERT(SIZEOF(.kickos_kernel_sdata) == 0,
       "KickOS: a KickOS archive emits .sdata/.sbss small data, which belongs to the app's gp window. Build every KickOS RISC-V lib -msmall-data-limit=0 (kickos_apply_freestanding in cmake/kickos.cmake)")
#if KICKOS_RV32_LOADS_AT_VMA
ASSERT(_appdata_lma == __kickos_appdata_start,
       "KickOS: .appdata LMA != VMA: the ROM loader places segments at their VMAs, so the LMA holds no image bytes (drop the AT clause)")
#endif
#endif

KICKOS_HEAP_MIN_ASSERT()

#if KICKOS_AMP_OWN_IMAGE
KICKOS_AMP_SHARED_ASSERT(ADDR(.amp_shared))
KICKOS_AMP_SLICE_ASSERT(ORIGIN(RAM), _estack, KICKOS_AMP_PARTITION_BASE, KICKOS_AMP_NODE_SHARE)
KICKOS_AMP_SHARED_FILLED_ASSERT()
#endif

#if !KICKOS_HAVE_MPU
KICKOS_APP_CODE_WINDOW_NONE()
KICKOS_APP_DATA_WINDOW_NONE()
#endif
KICKOS_APP_IMAGE_SPLIT_NONE()

#endif
