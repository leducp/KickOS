/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * NOT a C header: these macros expand to GNU ld statements, the body every Cortex-M chip
 * script shares between its own vector head and its own asserts. Regions are FLASH and RAM.
 */
#ifndef KICKOS_ARCH_ARM_COMMON_CORTEXM_LD_H
#define KICKOS_ARCH_ARM_COMMON_CORTEXM_LD_H

#include <sections.ld.h>

/* What .text holds after the vectors the chip places first. */
#define KICKOS_CORTEXM_TEXT                                                   \
    *(.text .text.*)                                                          \
    *(.rodata .rodata.*)                                                      \
    KICKOS_CODE_DEBRIS_SECTIONS                                               \
    KEEP(*(.init))                                                            \
    KEEP(*(.fini))                                                            \
    . = ALIGN(4);

/* EHABI unwind instructions and LSDA, homed in the code window the app's RX grant covers.
 * libstdc++ emits .gcc_except_table even on EHABI. */
#define KICKOS_CORTEXM_EH                                                     \
    .ARM.extab :                                                              \
    {                                                                         \
        *(.ARM.extab* .gnu.linkonce.armextab.*)                               \
    } > FLASH                                                                 \
    .gcc_except_table :                                                       \
    {                                                                         \
        *(.gcc_except_table .gcc_except_table.*)                              \
    } > FLASH

#define KICKOS_CORTEXM_EXIDX                                                  \
    .ARM.exidx :                                                              \
    {                                                                         \
        __exidx_start = .;                                                    \
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)                               \
        __exidx_end = .;                                                      \
    } > FLASH

/* .init_array holds ONLY the closed KickOS-owned set, which Reset_Handler walks before kmain;
 * every other ctor lands in .kickos_app_init_array, run from root_entry with the kernel live,
 * since a ctor may make a KickOS call. The split is by input ARCHIVE: init_priority(N) emits
 * .init_array.0NNNN, which no section name selector would keep apart.
 *
 * .fini_array is asserted empty and nothing walks it: under -fno-use-cxa-atexit a non-trivial
 * static destructor registers there, and an unplaced entry would be gc'd or orphaned outside
 * every copied range. It stays app-side, so a future teardown runs unprivileged.
 */
#define KICKOS_CORTEXM_CTORS                                                  \
    .init_array :                                                             \
    {                                                                         \
        __init_array_start = .;                                               \
        KEEP(*libkickos_kernel.a:*(SORT(.init_array.*) .init_array))          \
        KEEP(*KICKOS_LD_ARCH_LIB:*(SORT(.init_array.*) .init_array))          \
        KEEP(*KICKOS_LD_CHIP_LIB:*(SORT(.init_array.*) .init_array))          \
        KEEP(*libkickos_lib.a:*(SORT(.init_array.*) .init_array))             \
        __init_array_end = .;                                                 \
    } > FLASH                                                                 \
    .kickos_app_fini_array :                                                  \
    {                                                                         \
        __kickos_app_fini_array_start = .;                                    \
        KEEP(*(SORT(.fini_array.*)))                                          \
        KEEP(*(.fini_array))                                                  \
        __kickos_app_fini_array_end = .;                                      \
    } > FLASH                                                                 \
    KICKOS_TLS_TEMPLATE(FLASH)                                                \
    .kickos_app_init_array :                                                  \
    {                                                                         \
        __kickos_app_init_array_start = .;                                    \
        KEEP(*(SORT(.init_array.*)))                                          \
        KEEP(*(.init_array))                                                  \
        __kickos_app_init_array_end = .;                                      \
    } > FLASH

#define KICKOS_CORTEXM_ARENA                                                  \
    . = ALIGN(32);                                                            \
    __kickos_ram_start = .;                                                   \
    __kickos_ram_end = _estack - _kernel_stack_size;                          \
    __kickos_rom_start = ORIGIN(FLASH);                                       \
    __kickos_rom_end = ORIGIN(FLASH) + LENGTH(FLASH);                         \
    __kickos_sram_start = ORIGIN(RAM);

/* KICKOS_CORTEXM_DATA(appdata_head) lays out the writable state; appdata_head opens the app
 * window's output section, KICKOS_APPDATA_SECTION(_ebss) on a pow2 PMSA or
 * `.appdata : ALIGN(32)` on an MPU with a 32-byte granule.
 *
 * Under the MPU the selection is INVERTED: the closed KickOS-owned set is captured FIRST, in
 * script order, by `archive:member` selectors into the kernel .data/.bss at the RAM base, and
 * the .appdata/.appbss catch-alls take everything else, so an archive nobody listed lands
 * app-side (reachable) and never kernel-side (faults). EXCLUDE_FILE cannot stand in for the
 * colon syntax: this binutils does not match archive members inside it.
 *
 * .appbss is based at the window start plus SIZEOF(.appdata) and not at the location counter:
 * an EMPTY .appdata does not carry its ALIGN forward, and .appbss would fall below the window.
 * The pad is MAX(., ...) because ld evaluates ASSERTs after layout, and a pad moving backwards
 * would fail the link ahead of the overflow assert. The newlib heap IS the window pad, its base
 * aligned relative to .appbss (app_window.ld.h).
 *
 * Without the MPU no window is carved and the newlib heap is a section of its own, kept even
 * at zero length: _sbrk is force-linked into every image and names its start.
 */
#if KICKOS_HAVE_MPU
#define KICKOS_CORTEXM_DATA(appdata_head)                                     \
    _sidata = LOADADDR(.data);                                                \
    .data : ALIGN(4)                                                          \
    {                                                                         \
        _sdata = .;                                                           \
        *libkickos_kernel.a:*(.data .data.*)                                  \
        *KICKOS_LD_ARCH_LIB:*(.data .data.*)                                  \
        *KICKOS_LD_CHIP_LIB:*(.data .data.*)                                  \
        *libkickos_lib.a:*(.data .data.*)                                     \
        . = ALIGN(4);                                                         \
        _edata = .;                                                           \
    } > RAM AT > FLASH                                                        \
    .bss (NOLOAD) : ALIGN(4)                                                  \
    {                                                                         \
        _sbss = .;                                                            \
        *libkickos_kernel.a:*(.bss .bss.*)                                    \
        *KICKOS_LD_ARCH_LIB:*(.bss .bss.*)                                    \
        *KICKOS_LD_CHIP_LIB:*(.bss .bss.*)                                    \
        *libkickos_lib.a:*(.bss .bss.*)                                       \
        *libkickos_kernel.a:*(COMMON)                                         \
        *KICKOS_LD_ARCH_LIB:*(COMMON)                                         \
        *KICKOS_LD_CHIP_LIB:*(COMMON)                                         \
        *libkickos_lib.a:*(COMMON)                                            \
        . = ALIGN(4);                                                         \
        _ebss = .;                                                            \
    } > RAM                                                                   \
    appdata_head                                                              \
    {                                                                         \
        __kickos_appdata_start = .;                                           \
        *(.data .data.*)                                                      \
        . = ALIGN(8);                                                         \
        __kickos_appdata_load_end = .;                                        \
    } > RAM AT > FLASH                                                        \
    .appbss (__kickos_appdata_start + SIZEOF(.appdata)) (NOLOAD) : ALIGN(4)   \
    {                                                                         \
        *(.bss .bss.* COMMON)                                                 \
        . = ALIGN(4);                                                         \
        _appdata_used_end = .;                                                \
        . = MAX(., __kickos_appdata_start + _appdata_size);                   \
        __kickos_appdata_end = .;                                             \
        _kickos_heap_start = ALIGN(_appdata_used_end, 8);                     \
        _kickos_heap_limit = __kickos_appdata_end;                            \
    } > RAM                                                                   \
    KICKOS_CORTEXM_ARENA
#else
#define KICKOS_CORTEXM_DATA(appdata_head)                                     \
    _sidata = LOADADDR(.data);                                                \
    .data : ALIGN(4)                                                          \
    {                                                                         \
        _sdata = .;                                                           \
        *(.data .data.*)                                                      \
        . = ALIGN(4);                                                         \
        _edata = .;                                                           \
    } > RAM AT > FLASH                                                        \
    .bss (NOLOAD) : ALIGN(4)                                                  \
    {                                                                         \
        _sbss = .;                                                            \
        *(.bss .bss.*)                                                        \
        *(COMMON)                                                             \
        . = ALIGN(4);                                                         \
        _ebss = .;                                                            \
    } > RAM                                                                   \
    .userheap (NOLOAD) :                                                      \
    {                                                                         \
        . = ALIGN(8);                                                         \
        _kickos_heap_start = .;                                               \
        . += KICKOS_USER_HEAP_SIZE;                                           \
        _kickos_heap_limit = .;                                               \
    } > RAM                                                                   \
    KICKOS_CORTEXM_ARENA
#endif

#define KICKOS_CORTEXM_DISCARD                                                \
    /DISCARD/ : { *(.ARM.attributes) }                                        \
    KICKOS_NONALLOC_SECTIONS

/* An empty kernel .bss means the closed-set selectors matched nothing, kernel writable state
 * having leaked into the app window; an empty .appdata is legitimate. */
#if KICKOS_HAVE_MPU
#define KICKOS_CORTEXM_WINDOW_ASSERTS()                                       \
    ASSERT(_appdata_used_end <= __kickos_appdata_start + _appdata_size,       \
           "KickOS: app .data/.bss overflow _appdata_size (raise KICKOS_APPDATA_SIZE)") \
    ASSERT(ADDR(.appbss) >= __kickos_appdata_start,                           \
           "KickOS: .appbss placed below the app-data window base (empty .appdata alignment-drop regression)") \
    ASSERT(_ebss > _sbss,                                                     \
           "KickOS: kernel .bss empty: closed-set archive selector matched nothing (renamed lib?)")
#else
#define KICKOS_CORTEXM_WINDOW_ASSERTS() KICKOS_APP_DATA_WINDOW_NONE()
#endif

/* The file-scope asserts every Cortex-M image holds. The chip adds the arena asserts, its code
 * window or KICKOS_APP_CODE_WINDOW_NONE(), and any placement its boot ROM demands. */
#define KICKOS_CORTEXM_ASSERTS()                                              \
    KICKOS_STATIC_RELOC_ASSERT()                                              \
    KICKOS_TLS_ALIGN_ASSERT()                                                 \
    KICKOS_TLS_FIT_ASSERT()                                                   \
    ASSERT(__kickos_ram_start <= __kickos_ram_end,                            \
           "KickOS: static data overflows RAM (no room for the user-RAM pool + stack)") \
    KICKOS_FINI_EMPTY_ASSERT()                                                \
    KICKOS_CORTEXM_WINDOW_ASSERTS()                                           \
    KICKOS_HEAP_MIN_ASSERT()                                                  \
    KICKOS_APP_IMAGE_SPLIT_NONE()

#endif
