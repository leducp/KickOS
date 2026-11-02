/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * NOT a C header: the GNU ld body every RV32IMAC chip script includes after its MEMORY, which
 * names RAM (and SHARED for an own-image AMP node). The whole image runs from RAM, _start
 * (.text.init) first at its origin.
 *
 * KICKOS_RV32_LOADS_AT_VMA, which the chip defines, is 1 where the kernel's and the app's .data
 * load where they run (an esptool image): this script gives them no AT clause, the chip's
 * Reset_Handler copies from _sidata and _appdata_lma, and LMA == VMA is asserted. At 0 the
 * loader places each segment at its PhysAddr, .data's VMA is pinned past the code pad with its
 * bytes at the LMA, and kickos_ranges_init moves them by the copy table. Where each task runs an
 * image of its own, the image template says where each image's .data loads.
 *
 * Under KICKOS_HAVE_MPU the script carves what every UNPRIVILEGED thread needs under PMP
 * enforcement:
 *   __kickos_pmp_code_start/_end  every image's code, rodata and templates, the TOR pair the
 *                                 arch writes once at boot (entries 8 and 9, RX); the kernel's
 *                                 text lies below the start
 *   __kickos_appdata_start/_end   the app window, RW-NX, one pow2 NAPOT region
 * (arch_domain_static_regions). Where the images are not placed, the window is carved at
 * _kernel_data_top, a multiple of _appdata_size.
 *
 * The single gp small-data window (__global_pointer$ +/- 0x800) is anchored INSIDE .appdata:
 * the KickOS libs are built -msmall-data-limit=0, so the window holds only app and C++ runtime
 * small data and the app grant covers it.
 */
#ifndef KICKOS_ARCH_RISCV_COMMON_RV32IMAC_LD_H
#define KICKOS_ARCH_RISCV_COMMON_RV32IMAC_LD_H

/* 1 where the chip's image template loads every image's .data at its template. */
#ifndef KICKOS_RV32_IMAGE_DATA_LOADED
#define KICKOS_RV32_IMAGE_DATA_LOADED 0
#endif

/* The alignment of the code window's two ends, which the chip may raise. A section, so the
   sections placed > RAM after it, and the load addresses that follow, start past the pad. */
#ifndef KICKOS_RV32_CODE_WINDOW_ALIGN
#define KICKOS_RV32_CODE_WINDOW_ALIGN 4
#endif
#define KICKOS_RV32_CODE_WINDOW_PAD(name)                                     \
    name :                                                                    \
    {                                                                         \
        . = ALIGN(KICKOS_RV32_CODE_WINDOW_ALIGN);                             \
    } > RAM

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

#if KICKOS_HAVE_MPU
/* The app window's pad is the newlib heap: a 32K window leaves about 14K, room for the
   cxxtest peak (about 4.2K on RISC-V DWARF) and printf. */
#ifndef KICKOS_APPDATA_SIZE
#define KICKOS_APPDATA_SIZE 32K
#endif
_appdata_size = KICKOS_APPDATA_SIZE;
/* Two 32K windows: the kernel's .data/.bss runs a few hundred bytes past one. Read only where
   the images are not placed. */
#ifndef KICKOS_KERNEL_DATA_RESERVE
#define KICKOS_KERNEL_DATA_RESERVE 64K
#endif
#endif

SECTIONS
{
    .text ORIGIN(RAM) : ALIGN(4)
    {
        KEEP(*(.text.init))
        KICKOS_KERNEL_SECTIONS(.text .text.*)
        KICKOS_CODE_DEBRIS_SECTIONS
        KICKOS_KERNEL_SECTIONS(.rodata .rodata.*)
        KICKOS_KERNEL_SECTIONS(.srodata .srodata.*)
        . = ALIGN(4);
    } > RAM

    /* .init_array holds ONLY the closed KickOS-owned set, run by Reset_Handler; every other
       ctor runs from its image's startup with the kernel live. */
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

    KICKOS_IMAGE_TABLE(RAM, LONG)
    KICKOS_RV32_CODE_WINDOW_PAD(.kickos_code_window_lo)
    __kickos_kernel_code_end = .;

#if KICKOS_IMAGES_PLACED
    /* The thread-local template of every image, then every image's code span, which
       tools/image-link.py writes per link (KICKOS_RV32_IMAGE_CODE). */
    KICKOS_TLS_TEMPLATE(RAM)
    INCLUDE kickos_images_code.ld
    KICKOS_RV32_CODE_WINDOW_PAD(.kickos_code_window_hi)
    _code_used_end = .;
    __eh_frame_start = KICKOS_LD_IMAGE_SYM(1, eh_frame);
    __kickos_app_init_array_start = KICKOS_LD_IMAGE_SYM(1, ctor_start);
    __kickos_app_init_array_end = KICKOS_LD_IMAGE_SYM(1, ctor_end);
#else
    /* The app image's code span, then where the init is an image of its own the init's. DWARF EH,
       registered by each image's startup (user/src/image_start.cc): -nostartfiles drops
       crtbegin's frame_dummy and crtend's __FRAME_END__, so each table is kept whole and ends
       in the zero-length CIE crtend would supply. */
    KICKOS_APP_HEADER(RAM)
    .kickos_app_text : ALIGN(4)
    {
        KICKOS_APP_SECTIONS(.text .text.*)
        KICKOS_APP_SECTIONS(.rodata .rodata.*)
        KICKOS_APP_SECTIONS(.srodata .srodata.*)
        KICKOS_APP_SECTIONS(.gcc_except_table .gcc_except_table.*)
        . = ALIGN(4);
        KICKOS_LD_APP_SYM(eh_frame) = .;
        KEEP(KICKOS_APP_SECTIONS(.eh_frame))
        LONG(0)
        . = ALIGN(4);
    } > RAM
    __eh_frame_start = KICKOS_LD_APP_SYM(eh_frame);

    KICKOS_TLS_TEMPLATE(RAM)

    .kickos_app_init_array : ALIGN(4)
    {
        KICKOS_LD_APP_SYM(ctor_start) = .;
        KEEP(KICKOS_APP_SECTIONS(SORT(.init_array.*)))
        KEEP(KICKOS_APP_SECTIONS(.init_array))
        KICKOS_LD_APP_SYM(ctor_end) = .;
        . = ALIGN(4);
        _app_code_end = .;
    } > RAM
    __kickos_app_init_array_start = KICKOS_LD_APP_SYM(ctor_start);
    __kickos_app_init_array_end = KICKOS_LD_APP_SYM(ctor_end);

    KICKOS_RV32_CODE_WINDOW_PAD(.kickos_code_window_hi)
    _code_used_end = .;
#endif

#if KICKOS_HAVE_MPU
    /* Past the kernel's own text, which the TOR pair leaves out. A task's restart template
       lies in its code span. */
    __kickos_pmp_code_start = __kickos_kernel_code_end;
    __kickos_pmp_code_end = _code_used_end;
#if KICKOS_IMAGES_PLACED
    /* tools/image-link.py states __kickos_data_pin, where the code ended once relaxed, so a base
       computed from the code does not move as relaxation shrinks it. */
    PROVIDE(__kickos_data_pin = 0);
    _kernel_data_base = MAX(__kickos_data_pin, ALIGN(_code_used_end, 8));
#else
    _kernel_data_base = ALIGN(_code_used_end, _appdata_size);
    KICKOS_KERNEL_DATA_RESERVE_DECL(_kernel_data_base)
#endif
    KICKOS_APP_CODE_WINDOW_NONE()

#if KICKOS_RV32_LOADS_AT_VMA
    _sidata = LOADADDR(.data);
#endif

    /* THE SELECTION IS INVERTED: the kernel's inputs are captured FIRST into the kernel
       .data/.bss right after the code region, and .appdata/.appbss take image 1's, gp small
       data included, and each other image's window its own where the init is an image of its
       own. Kernel .data is pinned at _kernel_data_base and, where the images are not placed, the
       app window at _kernel_data_top, so the window, the heap and the arena sit at declared
       addresses rather than following kernel .bss over its alignment cliff.

       Small data from a KickOS lib is captured here, ahead of the image's, and refused at the
       end of this file. */
    .kickos_kernel_sdata :
    {
        *libkickos_kernel.a:*(.sdata* .sbss*)
        *KICKOS_LD_ARCH_LIB:*(.sdata* .sbss*)
        *KICKOS_LD_CHIP_LIB:*(.sdata* .sbss*)
        *libkickos_lib.a:*(.sdata* .sbss*)
        *kickos_string.dir*(.sdata* .sbss*)
    }

    .data _kernel_data_base : ALIGN(8)
    {
        _sdata = .;
        KICKOS_KERNEL_SECTIONS(.data .data.*)
        . = ALIGN(4);
        _edata = .;
    } > RAM KICKOS_RV32_LOAD

    .bss (NOLOAD) : ALIGN(4)
    {
        _sbss = .;
        __kernel_bss_start = .;
        KICKOS_KERNEL_SECTIONS(.bss .bss.*)
        KICKOS_KERNEL_SECTIONS(COMMON)
        __kernel_bss_end = .;
        . = ALIGN(4);
        _ebss = .;
    } > RAM

#if KICKOS_IMAGES_PLACED
    /* Every image's window, which tools/image-link.py writes per link
       (KICKOS_RV32_IMAGE_WINDOW), the first past kernel .bss. */
    INCLUDE kickos_images.ld
#else
    /* .appbss is pinned right after .appdata via SIZEOF, so the zero of
       [end of the loaded bytes, __kickos_appdata_end) covers the whole grant with no gap. */
    KICKOS_APPDATA_SECTION(_ebss)
    {
        __kickos_appdata_start = .;
        KICKOS_APP_SECTIONS(.data .data.*)
        . = ALIGN(8);
        __sdata_start = .;
        PROVIDE(__global_pointer$ = . + 0x800);
        KICKOS_APP_SECTIONS(.sdata .sdata.* .sdata2.*)
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
        KICKOS_APP_SECTIONS(.sbss .sbss.*)
        KICKOS_APP_SECTIONS(.bss .bss.* COMMON)
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
    KICKOS_LD_APP_SYM(window_end) = __kickos_appdata_end;
#endif
    KICKOS_IMAGE_BOUNDS(__kickos_pmp_code_start, __kickos_pmp_code_end,
                        ORIGIN(RAM), __kickos_kernel_code_end,
                        ORIGIN(RAM), ORIGIN(RAM) + LENGTH(RAM),
                        _sdata, _ebss, _estack - _kernel_stack_size, _estack,
                        __kickos_images_in_place)
#else
    __kickos_pmp_code_start = 0;
    __kickos_pmp_code_end = 0;
#if KICKOS_RV32_LOADS_AT_VMA
    _sidata = LOADADDR(.data);
#endif

    .data : ALIGN(8)
    {
        _sdata = .;
        KICKOS_KERNEL_SECTIONS(.data .data.*)
        KICKOS_KERNEL_SECTIONS(.sdata .sdata.* .sdata2.*)
        . = ALIGN(4);
        _edata = .;
    } > RAM KICKOS_RV32_LOAD

    .bss (NOLOAD) : ALIGN(4)
    {
        _sbss = .;
        KICKOS_KERNEL_SECTIONS(.sbss .sbss.*)
        KICKOS_KERNEL_SECTIONS(.bss .bss.*)
        KICKOS_KERNEL_SECTIONS(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

#if KICKOS_IMAGES_PLACED
    /* Every image's window, exact, which tools/image-link.py writes per link
       (KICKOS_RV32_IMAGE_WINDOW). */
    INCLUDE kickos_images.ld
#else
    /* The app image's window, exact: .data, gp small data, .bss and the heap. */
    .appdata : ALIGN(8)
    {
        KICKOS_APP_SECTIONS(.data .data.*)
        . = ALIGN(8);
        PROVIDE(__global_pointer$ = . + 0x800);
        KICKOS_APP_SECTIONS(.sdata .sdata.* .sdata2.*)
        . = ALIGN(8);
    } > RAM KICKOS_RV32_LOAD

    .appbss (ADDR(.appdata) + SIZEOF(.appdata)) (NOLOAD) : ALIGN(4)
    {
        KICKOS_APP_SECTIONS(.sbss .sbss.*)
        KICKOS_APP_SECTIONS(.bss .bss.* COMMON)
        . = ALIGN(4);
        _appdata_used_end = .;
    } > RAM

    .userheap (NOLOAD) :
    {
        . = ALIGN(8);
        _kickos_heap_start = .;
        . += KICKOS_USER_HEAP_SIZE;
        _kickos_heap_limit = .;
    } > RAM
    KICKOS_LD_APP_SYM(window_end) = _kickos_heap_limit;
#endif
    KICKOS_IMAGE_BOUNDS(ORIGIN(RAM), ORIGIN(RAM) + LENGTH(RAM),
                        ORIGIN(RAM), __kickos_kernel_code_end,
                        ORIGIN(RAM), ORIGIN(RAM) + LENGTH(RAM),
                        _sdata, _ebss, _estack - _kernel_stack_size, _estack,
                        __kickos_images_in_place)
#endif

#if KICKOS_IMAGES_PLACED
    /* Image 1's window and heap, the app's. Every image's .data loads as image 1's does, where it
       runs or at its template (the chip's image template). */
#if KICKOS_HAVE_MPU
    __kickos_appdata_start = KICKOS_LD_IMAGE_SYM(1, window_start);
    __kickos_appdata_end = KICKOS_LD_IMAGE_SYM(1, window_end);
#endif
    _appdata_used_end = KICKOS_LD_IMAGE_SYM(1, bss_end);
    _kickos_heap_start = KICKOS_LD_IMAGE_SYM(1, heap_start);
    _kickos_heap_limit = KICKOS_LD_IMAGE_SYM(1, heap_end);
    PROVIDE(__global_pointer$ = KICKOS_LD_IMAGE_SYM(1, window_start) + 0x800);
    __kickos_images_in_place = 1 - KICKOS_RV32_IMAGE_DATA_LOADED;
#else
    KICKOS_LD_APP_SYM(window_start) = ADDR(.appdata);
    KICKOS_LD_APP_SYM(data_start) = ADDR(.appdata);
    KICKOS_LD_APP_SYM(data_end) = ADDR(.appdata) + SIZEOF(.appdata);
    KICKOS_LD_APP_SYM(data_load) = LOADADDR(.appdata);
    KICKOS_LD_APP_SYM(bss_end) = _appdata_used_end;
    KICKOS_LD_APP_SYM(heap_start) = _kickos_heap_start;
    KICKOS_LD_APP_SYM(heap_end) = _kickos_heap_limit;
    /* Where .appdata loads where it runs, as the ESP32-C6 ROM and the QEMU ELF loader place it,
       its bytes are not in the code span. Every image's window loads the same way. */
    __kickos_images_in_place = LOADADDR(.appdata) == ADDR(.appdata);
    KICKOS_LD_APP_SYM(code_end) = _app_code_end + (1 - __kickos_images_in_place)
        * (LOADADDR(.appdata) + SIZEOF(.appdata) - _app_code_end);
#endif
    KICKOS_IMAGE0_TLS()
    KICKOS_IMAGE0_NAMES()

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
/* The chip states its .data loads where it runs, and its Reset_Handler copies by that. */
ASSERT(_sidata == _sdata,
       "KickOS: .data LMA != VMA where KICKOS_RV32_LOADS_AT_VMA states they meet (drop the AT clause)")
#endif

#if KICKOS_IMAGES_PLACED
ASSERT((LOADADDR(.kickos_image1_data) != ADDR(.kickos_image1_data)) == KICKOS_RV32_IMAGE_DATA_LOADED,
       "KickOS: KICKOS_RV32_IMAGE_DATA_LOADED disagrees with where the image template loads .data")
#endif

#if KICKOS_HAVE_MPU
#if !KICKOS_IMAGES_PLACED
ASSERT(_appdata_used_end <= __kickos_appdata_start + _appdata_size,
       "KickOS: app .data/.bss overflow _appdata_size (raise it to the next pow2)")
#endif
#if KICKOS_IMAGES_PLACED
INCLUDE kickos_images_asserts.ld
#endif
/* An empty kernel .bss means the closed-set selectors matched nothing, kernel writable state
   having leaked into the app catch-all. */
ASSERT(__kernel_bss_end > __kernel_bss_start,
       "KickOS: kernel .bss empty: closed-set archive selector matched nothing (renamed lib?)")
/* Past the psABI's gp reach, gp-relative loads resolve at the wrong addresses. Where each task
   runs an image of its own, the linker relaxes an access through gp only within its reach. */
#if !KICKOS_IMAGES_PLACED
ASSERT(__sdata_end <= __global_pointer$ + 0x800,
       "KickOS: runtime small-data overflows the gp + 0x800 reach")
ASSERT(__sdata_start >= __global_pointer$ - 0x800,
       "KickOS: runtime small-data underflows the gp - 0x800 reach")
#endif
ASSERT(SIZEOF(.kickos_kernel_sdata) == 0,
       "KickOS: a KickOS archive emits .sdata/.sbss small data, which belongs to the app's gp window. Build every KickOS RISC-V lib -msmall-data-limit=0 (kickos_apply_freestanding in cmake/kickos.cmake)")
#if KICKOS_RV32_LOADS_AT_VMA && !KICKOS_IMAGES_PLACED
ASSERT(_appdata_lma == __kickos_appdata_start,
       "KickOS: .appdata LMA != VMA where KICKOS_RV32_LOADS_AT_VMA states they meet (drop the AT clause)")
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
