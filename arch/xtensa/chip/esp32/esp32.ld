/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the ESP32 (WROOM-32 module), Xtensa LX6. The image links
 * ENTIRELY into internal SRAM (no flash cache / XIP), so the ROM loader copies
 * every segment to its RAM load address and jumps: no second stage, no MMU
 * setup (see startup.S). Addresses are from the ESP32 TRM v5.8, Table 3.3-2
 * "Embedded Memory Address Mapping":
 *
 *   Internal SRAM1 (IRAM1, exec): the kernel's vectors and text, from 0x400A_0000,
 *                                 the instruction alias of data 0x3FFF_FFFC down.
 *   Internal SRAM0 (IRAM, exec):  every image's text, in the upper 128 KiB from
 *                                 0x4008_0000 (the low 64 KiB is the cache region).
 *   Internal SRAM2 (DRAM, data):  0x3FFA_E000 .. 0x3FFD_FFFF (200 KiB), so data
 *                                 links at 0x3FFB_0000.
 *
 * IRAM is word-access only, so .rodata (byte-addressable strings/const data) lives
 * in DRAM, not with .text.
 *
 * Windowed ABI: the mandatory WindowOverflow/Underflow vectors are part of the
 * .vectors table placed at VECBASE (startup.S); they spill/fill to each thread's
 * OWN stack, so no dedicated exception stack is reserved here.
 */

#include <chip_layout.h>
#include "esp32_image.ld.h"

ENTRY(_start)

MEMORY
{
    KICKOS_LINK_IRAM1
    KICKOS_LINK_IRAM
    KICKOS_LINK_DRAM
}

_estack = ORIGIN(DRAM) + LENGTH(DRAM); /* core 0's boot stack top: top of DRAM */
_kernel_stack_size = 8K;               /* boot/interrupt stack reserve, PER CORE */

/* ONE BOOT STACK PER CORE, stacked downward from the top of DRAM. The reserve below
 * __kickos_ram_end grows with the count, which makes __kickos_ram_start <= __kickos_ram_end
 * the check that the second stack FITS. */
#if KICKOS_NUM_CORES > 1
_estack_core1 = _estack - _kernel_stack_size;
#endif

SECTIONS
{
    /* Xtensa exception/interrupt vectors at VECBASE. Must be 1 KiB aligned; the
       startup sets wsr.vecbase to _kickos_vectors. Placed at IRAM1 origin. */
    .vectors ORIGIN(IRAM1) :
    {
        _kickos_vectors = .;
        KEEP(*(.vectors))
        . = ALIGN(4);
    } > IRAM1

    .text : ALIGN(4)
    {
        KICKOS_KERNEL_SECTIONS(.literal .literal.*)
        KICKOS_KERNEL_SECTIONS(.text .text.*)
        KICKOS_CODE_DEBRIS_SECTIONS
        . = ALIGN(4);
    } > IRAM1

#if KICKOS_IMAGES_PLACED
    /* Every image's text, its code span, which tools/image-link.py writes per link
       (KICKOS_ESP32_IMAGE_TEXT). IRAM is word-access only, so its header, rodata and the rest of
       what it reads live in DRAM, past the kernel's. */
    INCLUDE kickos_images_code.ld
#else
    /* An image's code span is its text, the app's then the init's where it is an image of its
       own. IRAM is word-access only, so its header, rodata and the rest of what it reads live
       in DRAM, past the kernel's. */
    .kickos_app_text : ALIGN(4)
    {
        _app_text_start = .;
        KICKOS_APP_SECTIONS(.literal .literal.*)
        KICKOS_APP_SECTIONS(.text .text.*)
        . = ALIGN(4);
        _app_text_end = .;
    } > IRAM
#endif

    .rodata : ALIGN(4)
    {
        KICKOS_KERNEL_SECTIONS(.rodata .rodata.*)
        KICKOS_KERNEL_SECTIONS(.rodata_wlog .rodata_wlog.*)
        . = ALIGN(4);
    } > DRAM

    /* Global ctors SPLIT: .init_array keeps ONLY the closed KickOS-owned set
       (kernel/arch/chip/lib), run early by Reset_Handler; other (app/lib) ctors
       land in each image's constructor window and run from its startup with the kernel live.
       The xtensa GCC, configured without --enable-initfini-array (conan/toolchain), emits
       .ctors: a table section no selector names is dropped by --gc-sections, constructor and
       all. SORT_BY_INIT_PRIORITY reads either spelling's priority for a forward walk. */
    .init_array : ALIGN(4)
    {
        __init_array_start = .;
        KEEP(*libkickos_kernel.a:*(SORT_BY_INIT_PRIORITY(.init_array.*) .init_array))
        KEEP(*libkickos_kernel.a:*(SORT_BY_INIT_PRIORITY(.ctors.*) .ctors))
        KEEP(*libkickos_arch_lx6.a:*(SORT_BY_INIT_PRIORITY(.init_array.*) .init_array))
        KEEP(*libkickos_arch_lx6.a:*(SORT_BY_INIT_PRIORITY(.ctors.*) .ctors))
        KEEP(*libkickos_chip_esp32.a:*(SORT_BY_INIT_PRIORITY(.init_array.*) .init_array))
        KEEP(*libkickos_chip_esp32.a:*(SORT_BY_INIT_PRIORITY(.ctors.*) .ctors))
        KEEP(*libkickos_lib.a:*(SORT_BY_INIT_PRIORITY(.init_array.*) .init_array))
        KEEP(*libkickos_lib.a:*(SORT_BY_INIT_PRIORITY(.ctors.*) .ctors))
        __init_array_end = .;
    } > DRAM

    /* .fini_array, app side, ASSERTed empty below; nothing walks it. See
       arch/arm/chip/stm32f411/stm32f411.ld. */
    .kickos_app_fini_array : ALIGN(4)
    {
        __kickos_app_fini_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP(*(.fini_array .dtors))
        __kickos_app_fini_array_end = .;
    } > DRAM

    KICKOS_IMAGE_TABLE(DRAM, LONG)

#if KICKOS_IMAGES_PLACED
    /* The kernel's own DWARF EH, kept whole and ended in a zero-length CIE, the thread-local
       template of every image, then what the ROM loads of every image: header, rodata,
       constructor window and .data template (KICKOS_ESP32_IMAGE), which tools/image-link.py
       writes per link. Every byte the ROM loads in DRAM is one span up to the kernel's .data, so
       esptool makes it one segment however many images there are. */
    .kickos_kernel_eh_frame : ALIGN(4)
    {
        KEEP(KICKOS_KERNEL_SECTIONS(.eh_frame))
        LONG(0)
        . = ALIGN(8);
    } > DRAM

    KICKOS_TLS_TEMPLATE(DRAM)

    INCLUDE kickos_images.ld
#else
    /* DWARF EH, registered by each image's startup (user/src/image_start.cc): -nostartfiles
       drops crtbegin's frame_dummy and crtend's terminator, so each table is kept whole and
       ends in a zero-length CIE. The kernel's own entries ride the app's. */
    KICKOS_APP_HEADER(DRAM)
    .kickos_app_rodata : ALIGN(4)
    {
        KICKOS_APP_SECTIONS(.rodata .rodata.*)
        KICKOS_APP_SECTIONS(.rodata_wlog .rodata_wlog.*)
        KEEP(KICKOS_APP_SECTIONS(.gcc_except_table .gcc_except_table.*))
        . = ALIGN(4);
        KICKOS_LD_APP_SYM(eh_frame) = .;
        KEEP(KICKOS_APP_SECTIONS(.eh_frame))
        KEEP(KICKOS_KERNEL_SECTIONS(.eh_frame))
        LONG(0)
        . = ALIGN(4);
    } > DRAM
    __eh_frame_start = KICKOS_LD_APP_SYM(eh_frame);

    KICKOS_TLS_TEMPLATE(DRAM)

    .kickos_app_init_array : ALIGN(4)
    {
        KICKOS_LD_APP_SYM(ctor_start) = .;
        KEEP(KICKOS_APP_SECTIONS(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(KICKOS_APP_SECTIONS(.init_array .ctors))
        KICKOS_LD_APP_SYM(ctor_end) = .;
    } > DRAM
    __kickos_app_init_array_start = KICKOS_LD_APP_SYM(ctor_start);
    __kickos_app_init_array_end = KICKOS_LD_APP_SYM(ctor_end);
#endif

    _sidata = LOADADDR(.data);

    .data : ALIGN(4)
    {
        _sdata = .;
        KICKOS_KERNEL_SECTIONS(.data .data.*)
        . = ALIGN(4);
        _edata = .;
    } > DRAM

    .bss (NOLOAD) : ALIGN(4)
    {
        _sbss = .;
        KICKOS_KERNEL_SECTIONS(.bss .bss.*)
        KICKOS_KERNEL_SECTIONS(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > DRAM

#if KICKOS_IMAGES_PLACED
    /* Every image's window (KICKOS_ESP32_WINDOW), which tools/image-link.py writes per link,
       image 1's named as the app's below. */
    INCLUDE kickos_images_windows.ld
    __eh_frame_start = KICKOS_LD_IMAGE_SYM(1, eh_frame);
    __kickos_app_init_array_start = KICKOS_LD_IMAGE_SYM(1, ctor_start);
    __kickos_app_init_array_end = KICKOS_LD_IMAGE_SYM(1, ctor_end);
    _appdata_used_end = KICKOS_LD_IMAGE_SYM(1, bss_end);
    _kickos_heap_start = KICKOS_LD_IMAGE_SYM(1, heap_start);
    _kickos_heap_limit = KICKOS_LD_IMAGE_SYM(1, heap_end);
#else
    /* The app image's window, exact: .data, .bss and the heap. */
    .appdata : ALIGN(8)
    {
        KICKOS_APP_SECTIONS(.data .data.*)
        . = ALIGN(8);
    } > DRAM

    .appbss (ADDR(.appdata) + SIZEOF(.appdata)) (NOLOAD) : ALIGN(4)
    {
        KICKOS_APP_SECTIONS(.bss .bss.* COMMON)
        . = ALIGN(4);
        _appdata_used_end = .;
    } > DRAM

    .userheap (NOLOAD) :
    {
        . = ALIGN(8);
        _kickos_heap_start = .;
        . += KICKOS_USER_HEAP_SIZE;
        _kickos_heap_limit = .;
    } > DRAM


    KICKOS_LD_APP_SYM(code_start) = _app_text_start;
    KICKOS_LD_APP_SYM(code_end) = _app_text_end;
    KICKOS_LD_APP_SYM(window_start) = ADDR(.appdata);
    KICKOS_LD_APP_SYM(window_end) = _kickos_heap_limit;
    KICKOS_LD_APP_SYM(data_load) = LOADADDR(.appdata);
    KICKOS_LD_APP_SYM(data_start) = ADDR(.appdata);
    KICKOS_LD_APP_SYM(data_end) = ADDR(.appdata) + SIZEOF(.appdata);
    KICKOS_LD_APP_SYM(bss_end) = _appdata_used_end;
    KICKOS_LD_APP_SYM(heap_start) = _kickos_heap_start;
    KICKOS_LD_APP_SYM(heap_end) = _kickos_heap_limit;
#endif
    KICKOS_IMAGE0_TLS()
    KICKOS_IMAGE0_NAMES()

    /* User-RAM pool (arch_ram_alloc) between static data and the reserved stack. */
    . = ALIGN(16);
    __kickos_ram_start = .;
    __kickos_ram_end = _estack - KICKOS_NUM_CORES * _kernel_stack_size;

    /* Chip memory extents read by the non-enforcing user-pointer admission
       (arch_ram_common.cc). Symbols only, no layout effect. */
    __kickos_rom_start = ORIGIN(IRAM);
    __kickos_rom_end = ORIGIN(IRAM1) + LENGTH(IRAM1);
    __kickos_sram_start = ORIGIN(DRAM);
    KICKOS_IMAGE_BOUNDS(ORIGIN(IRAM), ORIGIN(IRAM) + LENGTH(IRAM),
                        ORIGIN(IRAM1), ADDR(.text) + SIZEOF(.text),
                        ORIGIN(DRAM), ORIGIN(DRAM) + LENGTH(DRAM),
                        _sdata, _ebss, __kickos_ram_end, _estack, 1 - KICKOS_IMAGES_PLACED)

    /DISCARD/ : { *(.xtensa.info) *(.comment) *(.xt.*) }

    KICKOS_NONALLOC_SECTIONS
}

KICKOS_STATIC_RELOC_ASSERT()
KICKOS_TLS_ALIGN_ASSERT()
KICKOS_TLS_FIT_ASSERT()
#if KICKOS_IMAGES_PLACED
INCLUDE kickos_images_asserts.ld
#endif

ASSERT(__kickos_ram_start <= __kickos_ram_end,
       "KickOS: static data overflows DRAM (no room for the user-RAM pool + stack)")

#include <boot_arena.ld.h>
KICKOS_BOOT_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)

KICKOS_POOL_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)

KICKOS_FINI_EMPTY_ASSERT()

KICKOS_HEAP_MIN_ASSERT()

/* --- The coherency condition, as a refusal ---------------------------------------------
 * Both CPUs reach embedded memory with no cache in the path: the two 32 KiB caches are
 * carved out of SRAM0 and serve the external flash and PSRAM path alone, and the TRM
 * documents no invalidate anywhere and no flush but a whole-cache one that DISCARDS dirty
 * data. Shared kernel state is coherent across the two CPUs only while it is IN internal
 * SRAM. Unconditional, not gated on the core count.
 *
 * The bounds are the SoC's, from TRM v5.8 Table 3.3-2, and NOT this script's own regions:
 * an assert reading ORIGIN(DRAM) against LENGTH(DRAM) passes wherever the region is
 * repointed.
 *
 * Data bus: SRAM2 opens at 0x3FFA_E000 and SRAM1 closes at 0x3FFF_FFFF, contiguous.
 * Instruction bus: the low 64 KiB of SRAM0 IS the cache pool, so the floor is 0x4008_0000
 * rather than SRAM0's own 0x4007_0000, and SRAM1's instruction alias closes at 0x400B_FFFF.
 */
#define KICKOS_ESP32_ISRAM_DATA_FLOOR  0x3FFAE000
#define KICKOS_ESP32_ISRAM_DATA_CEIL   0x40000000
#define KICKOS_ESP32_ISRAM_TEXT_FLOOR  0x40080000
#define KICKOS_ESP32_ISRAM_TEXT_CEIL   0x400C0000

ASSERT(ADDR(.rodata) >= KICKOS_ESP32_ISRAM_DATA_FLOOR && _estack <= KICKOS_ESP32_ISRAM_DATA_CEIL,
       "KickOS: kernel data leaves internal SRAM. Run queues, control blocks, the capability table and the kernel lock counters are coherent across the two CPUs only by there being no cache over them, and the caches sit on the external path; a shared kernel over this placement loses writes with no fault")

ASSERT(__kickos_ram_end >= KICKOS_ESP32_ISRAM_DATA_FLOOR && _estack <= KICKOS_ESP32_ISRAM_DATA_CEIL,
       "KickOS: a boot stack leaves internal SRAM. Every core's boot and interrupt stack is carved below _estack, and the level-1 entry spills the interruptee's register windows onto whichever stack it is standing on; behind a cache neither core can invalidate, that spill is lost rather than faulted")

ASSERT(_kickos_vectors >= KICKOS_ESP32_ISRAM_TEXT_FLOOR && ADDR(.text) + SIZEOF(.text) <= KICKOS_ESP32_ISRAM_TEXT_CEIL,
       "KickOS: kernel text leaves internal SRAM, or reaches into the SRAM0 cache pool. Both CPUs fetch it directly and neither can invalidate the other's copy; the only flush this part has discards dirty data")

/* SRAM1 is one set of words on two buses, the instruction alias word-reversed against the data
 * one (TRM v5.8 3.3.2.4). The ROM keeps its data and both CPUs' stacks at data 0x3FFE_0000 to
 * 0x3FFE_7E30 (`__stack_app` in the ROM ELF), the alias of instruction 0x400B_81D0 up: the loader
 * runs on them while it loads, and APP_CPU's ROM entry when core 1 is released.
 */
#define KICKOS_ESP32_SRAM1_DATA_FLOOR  0x3FFE0000
#define KICKOS_ESP32_ROM_SRAM1_TEXT_FLOOR 0x400B81D0

ASSERT(ADDR(.text) + SIZEOF(.text) <= KICKOS_ESP32_ROM_SRAM1_TEXT_FLOOR,
       "KickOS: kernel text reaches the SRAM1 words the ROM loader runs on; the boot loads over the loader and never reaches _start")

ASSERT(_estack <= KICKOS_ESP32_SRAM1_DATA_FLOOR,
       "KickOS: data reaches SRAM1, whose words are the kernel text's and the ROM's")

ASSERT(ORIGIN(IRAM) + LENGTH(IRAM) == ORIGIN(IRAM1),
       "KickOS: IRAM and IRAM1 are not one span, which __kickos_rom_start/_end admit as code")

/* The app windows this script does not carve, stated EMPTY rather than left absent:
   the kernel's references are strong, so an omitted bound is a link error. */
KICKOS_APP_CODE_WINDOW_NONE()
KICKOS_APP_DATA_WINDOW_NONE()
KICKOS_APP_IMAGE_SPLIT_NONE()
