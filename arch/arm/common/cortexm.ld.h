/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * NOT a C header: these macros expand to GNU ld statements, the body every Cortex-M chip
 * script shares between its own vector head and its own asserts. Regions are FLASH and RAM.
 */
#ifndef KICKOS_ARCH_ARM_COMMON_CORTEXM_LD_H
#define KICKOS_ARCH_ARM_COMMON_CORTEXM_LD_H

#include <sections.ld.h>

/* The vector table is a data object nothing references. Not in an image template
 * (KICKOS_LD_TEMPLATE), which tools/image-link.py includes inside SECTIONS. */
#if !KICKOS_LD_TEMPLATE
EXTERN(g_isr_vector)
#endif

/* What .text holds after the vectors the chip places first: the kernel's code. */
#define KICKOS_CORTEXM_TEXT                                                   \
    KICKOS_KERNEL_SECTIONS(.text .text.*)                                     \
    KICKOS_KERNEL_SECTIONS(.rodata .rodata.*)                                 \
    KICKOS_CODE_DEBRIS_SECTIONS                                               \
    KEEP(*(.init))                                                            \
    KEEP(*(.fini))                                                            \
    . = ALIGN(4);

/* EHABI unwind instructions and LSDA. libstdc++ emits .gcc_except_table even on EHABI. The
 * kernel's here, each image's in its own code span (KICKOS_CORTEXM_APP_CODE). */
#define KICKOS_CORTEXM_EH                                                     \
    .ARM.extab :                                                              \
    {                                                                         \
        KICKOS_KERNEL_SECTIONS(.ARM.extab* .gnu.linkonce.armextab.*)          \
    } > FLASH                                                                 \
    .gcc_except_table :                                                       \
    {                                                                         \
        KICKOS_KERNEL_SECTIONS(.gcc_except_table .gcc_except_table.*)         \
    } > FLASH

#define KICKOS_CORTEXM_EXIDX                                                  \
    .ARM.exidx :                                                              \
    {                                                                         \
        KICKOS_KERNEL_SECTIONS(.ARM.exidx* .gnu.linkonce.armexidx.*)          \
    } > FLASH                                                                 \
    KICKOS_IMAGE_TABLE(FLASH, LONG)

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
    } > FLASH

/* An image's text and rodata, then its unwind tables, `name` naming its output sections and
 * `sections` selecting its inputs. */
#define KICKOS_CORTEXM_IMAGE_TEXT(n, name, sections)                          \
    .kickos_##name##_text :                                                   \
    {                                                                         \
        sections(.text .text.*)                                               \
        sections(.rodata .rodata.*)                                           \
        . = ALIGN(4);                                                         \
    } > FLASH                                                                 \
    .kickos_##name##_extab :                                                  \
    {                                                                         \
        sections(.ARM.extab* .gnu.linkonce.armextab.*)                        \
        sections(.gcc_except_table .gcc_except_table.*)                       \
    } > FLASH                                                                 \
    .kickos_##name##_exidx :                                                  \
    {                                                                         \
        KICKOS_LD_IMAGE_SYM(n, exidx_start) = .;                              \
        sections(.ARM.exidx* .gnu.linkonce.armexidx.*)                        \
        KICKOS_LD_IMAGE_SYM(n, exidx_end) = .;                                \
    } > FLASH                                                                 \
    KICKOS_LD_IMAGE_SYM(n, eh_frame) = 0;

/* The app image's code span: its header, text and rodata, unwind tables, the TLS template of
 * every image, its constructor window and, last, its .data load image. */
#define KICKOS_CORTEXM_APP_CODE                                               \
    KICKOS_APP_HEADER(FLASH)                                                  \
    KICKOS_CORTEXM_IMAGE_TEXT(KICKOS_APP_IMAGE_N, app, KICKOS_APP_SECTIONS)   \
    KICKOS_TLS_TEMPLATE(FLASH)                                                \
    .kickos_app_init_array :                                                  \
    {                                                                         \
        KICKOS_LD_APP_SYM(ctor_start) = .;                                    \
        KEEP(KICKOS_APP_SECTIONS(SORT(.init_array.*)))                        \
        KEEP(KICKOS_APP_SECTIONS(.init_array))                                \
        KICKOS_LD_APP_SYM(ctor_end) = .;                                      \
    } > FLASH                                                                 \
    __exidx_start = KICKOS_LD_APP_SYM(exidx_start);                           \
    __exidx_end = KICKOS_LD_APP_SYM(exidx_end);                               \
    __kickos_app_init_array_start = KICKOS_LD_APP_SYM(ctor_start);            \
    __kickos_app_init_array_end = KICKOS_LD_APP_SYM(ctor_end);                \
    KICKOS_IMAGE0_TLS()

/* Where the init is an image of its own: every image's asserts, which tools/image-link.py writes
 * per link with their code spans and windows (KICKOS_CORTEXM_IMAGE_CODE, KICKOS_CORTEXM_IMAGE).
 */
#if KICKOS_IMAGES_PLACED
#define KICKOS_CORTEXM_IMAGES_ASSERTS() INCLUDE kickos_images_asserts.ld
#else
#define KICKOS_CORTEXM_IMAGES_ASSERTS()
#endif

/* Image n's code span: its header, text and rodata, unwind tables and constructor window, then
 * the room its window's .data loads into. The image template (image.ld) instantiates it. */
#define KICKOS_CORTEXM_IMAGE_CODE(n)                                          \
    .kickos_image##n##_header : ALIGN(4)                                      \
    {                                                                         \
        KICKOS_LD_IMAGE_SYM(n, code_start) = .;                               \
        KEEP(*kickos_image##n.o(.kickos_image_header))                        \
    } > FLASH                                                                 \
    .kickos_image##n##_text :                                                 \
    {                                                                         \
        *kickos_image##n.o(.text .text.*)                                     \
        *kickos_image##n.o(.rodata .rodata.*)                                 \
        . = ALIGN(4);                                                         \
    } > FLASH                                                                 \
    .kickos_image##n##_extab :                                                \
    {                                                                         \
        *kickos_image##n.o(.ARM.extab* .gnu.linkonce.armextab.*)              \
        *kickos_image##n.o(.gcc_except_table .gcc_except_table.*)             \
    } > FLASH                                                                 \
    .kickos_image##n##_exidx :                                                \
    {                                                                         \
        KICKOS_LD_IMAGE_SYM(n, exidx_start) = .;                              \
        *kickos_image##n.o(.ARM.exidx* .gnu.linkonce.armexidx.*)              \
        KICKOS_LD_IMAGE_SYM(n, exidx_end) = .;                                \
    } > FLASH                                                                 \
    KICKOS_LD_IMAGE_SYM(n, eh_frame) = 0;                                     \
    .kickos_image##n##_init_array :                                           \
    {                                                                         \
        KICKOS_LD_IMAGE_SYM(n, ctor_start) = .;                               \
        KEEP(*kickos_image##n.o(SORT(.init_array.*)))                         \
        KEEP(*kickos_image##n.o(.init_array))                                 \
        KICKOS_LD_IMAGE_SYM(n, ctor_end) = .;                                 \
    } > FLASH                                                                 \
    .kickos_image##n##_dload (NOLOAD) : ALIGN(4)                              \
    {                                                                         \
        . += SIZEOF(.kickos_image##n##_data);                                 \
    } > FLASH                                                                 \
    KICKOS_LD_IMAGE_SYM(n, code_end) =                                        \
        ADDR(.kickos_image##n##_dload) + SIZEOF(.kickos_image##n##_dload);

/* Image n's window at `base`, or where RAM has room, aligned to `align`: .data loaded from the
 * room at the end of its code span, .bss, `heap` more bytes, then the window shaped by
 * KICKOS_IMAGE_WINDOW_END; its heap is the window past .bss. */
#define KICKOS_CORTEXM_IMAGE(n, base, align, heap)                            \
    .kickos_image##n##_data base : AT(ADDR(.kickos_image##n##_dload)) ALIGN(align) \
    {                                                                         \
        KICKOS_LD_IMAGE_SYM(n, window_start) = .;                             \
        *kickos_image##n.o(.data .data.*)                                     \
        . = ALIGN(8);                                                         \
    } > RAM                                                                   \
    .kickos_image##n##_bss (KICKOS_LD_IMAGE_SYM(n, window_start)              \
                            + SIZEOF(.kickos_image##n##_data)) (NOLOAD) : ALIGN(4) \
    {                                                                         \
        *kickos_image##n.o(.bss .bss.* COMMON)                                \
        . = ALIGN(4);                                                         \
        KICKOS_LD_IMAGE_SYM(n, bss_end) = .;                                  \
        . = ALIGN(8);                                                         \
        . += (heap);                                                          \
        KICKOS_IMAGE_WINDOW_END(KICKOS_LD_IMAGE_SYM(n, window_start))         \
        KICKOS_LD_IMAGE_SYM(n, window_end) = .;                               \
    } > RAM                                                                   \
    KICKOS_LD_IMAGE_SYM(n, data_load) = LOADADDR(.kickos_image##n##_data);    \
    KICKOS_LD_IMAGE_SYM(n, data_start) = ADDR(.kickos_image##n##_data);       \
    KICKOS_LD_IMAGE_SYM(n, data_end) =                                        \
        ADDR(.kickos_image##n##_data) + SIZEOF(.kickos_image##n##_data);      \
    KICKOS_LD_IMAGE_SYM(n, heap_start) = ALIGN(KICKOS_LD_IMAGE_SYM(n, bss_end), 8); \
    KICKOS_LD_IMAGE_SYM(n, heap_end) =                                        \
        MAX(KICKOS_LD_IMAGE_SYM(n, window_end), KICKOS_LD_IMAGE_SYM(n, heap_start)); \
    KICKOS_IMAGE_WINDOW_SIZE(n)                                               \
    KICKOS_IMAGE_NAMES(n)

/* The first window's base: past kernel .bss and the reserve kernel_data_reserve.ld.h pins. */
#define KICKOS_CORTEXM_WINDOWS_BASE MAX(KICKOS_LD_C_SYM(_ebss), _kernel_data_top)

#define KICKOS_CORTEXM_ARENA                                                  \
    . = ALIGN(32);                                                            \
    __kickos_ram_start = .;                                                   \
    __kickos_ram_end = _estack - _kernel_stack_size;                          \
    __kickos_rom_start = ORIGIN(FLASH);                                       \
    __kickos_rom_end = ORIGIN(FLASH) + LENGTH(FLASH);                         \
    __kickos_sram_start = ORIGIN(RAM);                                        \
    KICKOS_IMAGE_BOUNDS(ORIGIN(FLASH), ORIGIN(FLASH) + LENGTH(FLASH),         \
                        ORIGIN(FLASH), LOADADDR(.data) + SIZEOF(.data),       \
                        ORIGIN(RAM), ORIGIN(RAM) + LENGTH(RAM),               \
                        _sdata, _ebss, __kickos_ram_end, _estack, 0)          \
    KICKOS_IMAGE0_NAMES()

/* The app image's window after its code span: .data, .bss, then the heap. */
#define KICKOS_CORTEXM_APP_WINDOW_SYMBOLS(end)                                \
    KICKOS_LD_APP_SYM(window_start) = ADDR(.appdata);                         \
    KICKOS_LD_APP_SYM(window_end) = (end);                                    \
    KICKOS_LD_APP_SYM(data_load) = LOADADDR(.appdata);                        \
    KICKOS_LD_APP_SYM(data_start) = ADDR(.appdata);                           \
    KICKOS_LD_APP_SYM(data_end) = ADDR(.appdata) + SIZEOF(.appdata);          \
    KICKOS_LD_APP_SYM(bss_end) = _appdata_used_end;                           \
    KICKOS_LD_APP_SYM(heap_start) = _kickos_heap_start;                       \
    KICKOS_LD_APP_SYM(heap_end) = _kickos_heap_limit;                         \
    KICKOS_LD_APP_SYM(code_end) = LOADADDR(.appdata) + SIZEOF(.appdata);

/* KICKOS_CORTEXM_DATA(appdata_head) lays out the kernel's writable state, the app image's code
 * span and then its window, then the init image's where it is one of its own; appdata_head opens
 * the app window's output section, KICKOS_APPDATA_SECTION(_ebss) on a pow2 PMSA or
 * `.appdata : ALIGN(32)` on an MPU with a 32-byte granule.
 *
 * Under the MPU the selection is INVERTED: the kernel's inputs are captured FIRST, in script
 * order, into the kernel .data/.bss at the RAM base, and the .appdata/.appbss sections take
 * the app image's. EXCLUDE_FILE cannot stand in for the colon syntax over an archive member: this
 * binutils does not match archive members inside it.
 *
 * .appbss is based at the window start plus SIZEOF(.appdata) and not at the location counter:
 * an EMPTY .appdata does not carry its ALIGN forward, and .appbss would fall below the window.
 * The pad is MAX(., ...) because ld evaluates ASSERTs after layout, and a pad moving backwards
 * would fail the link ahead of the overflow assert. The newlib heap IS the window pad, its base
 * aligned relative to .appbss (app_window.ld.h).
 *
 * Without the MPU the window is exact: .data, .bss and a heap of KICKOS_USER_HEAP_SIZE, kept
 * even at zero length since _sbrk is force-linked into every image and names its start.
 */
#if KICKOS_IMAGES_PLACED
/* Where the init is an image of its own: the kernel's writable state, the thread-local template
 * of every image, then every image's code span and every image's window, which
 * tools/image-link.py writes per link (kickos_images_code.ld, kickos_images.ld), image 1's named
 * as the app's below. appdata_head is not read: no window is the app's alone. */
#if KICKOS_HAVE_MPU
#define KICKOS_CORTEXM_APP_WINDOW_NAMES()                                     \
    __kickos_appdata_start = KICKOS_LD_IMAGE_SYM(1, window_start);            \
    __kickos_appdata_end = KICKOS_LD_IMAGE_SYM(1, window_end);
#else
#define KICKOS_CORTEXM_APP_WINDOW_NAMES()
#endif
#define KICKOS_CORTEXM_DATA(appdata_head)                                     \
    _sidata = LOADADDR(.data);                                                \
    .data : ALIGN(4)                                                          \
    {                                                                         \
        _sdata = .;                                                           \
        KICKOS_KERNEL_SECTIONS(.data .data.*)                                 \
        . = ALIGN(4);                                                         \
        _edata = .;                                                           \
    } > RAM AT > FLASH                                                        \
    .bss (NOLOAD) : ALIGN(4)                                                  \
    {                                                                         \
        _sbss = .;                                                            \
        KICKOS_KERNEL_SECTIONS(.bss .bss.*)                                   \
        KICKOS_KERNEL_SECTIONS(COMMON)                                        \
        KICKOS_KERNEL_SECTIONS(.kickos_bss_last.*)                            \
        . = ALIGN(4);                                                         \
        _ebss = .;                                                            \
    } > RAM                                                                   \
    KICKOS_TLS_TEMPLATE(FLASH)                                                \
    KICKOS_IMAGE0_TLS()                                                       \
    INCLUDE kickos_images_code.ld                                             \
    INCLUDE kickos_images.ld                                                  \
    __exidx_start = KICKOS_LD_IMAGE_SYM(1, exidx_start);                      \
    __exidx_end = KICKOS_LD_IMAGE_SYM(1, exidx_end);                          \
    __kickos_app_init_array_start = KICKOS_LD_IMAGE_SYM(1, ctor_start);       \
    __kickos_app_init_array_end = KICKOS_LD_IMAGE_SYM(1, ctor_end);           \
    KICKOS_CORTEXM_APP_WINDOW_NAMES()                                         \
    _appdata_used_end = KICKOS_LD_IMAGE_SYM(1, bss_end);                      \
    _kickos_heap_start = KICKOS_LD_IMAGE_SYM(1, heap_start);                  \
    _kickos_heap_limit = KICKOS_LD_IMAGE_SYM(1, heap_end);                    \
    KICKOS_CORTEXM_ARENA
#elif KICKOS_HAVE_MPU
#define KICKOS_CORTEXM_DATA(appdata_head)                                     \
    _sidata = LOADADDR(.data);                                                \
    .data : ALIGN(4)                                                          \
    {                                                                         \
        _sdata = .;                                                           \
        KICKOS_KERNEL_SECTIONS(.data .data.*)                                 \
        . = ALIGN(4);                                                         \
        _edata = .;                                                           \
    } > RAM AT > FLASH                                                        \
    .bss (NOLOAD) : ALIGN(4)                                                  \
    {                                                                         \
        _sbss = .;                                                            \
        KICKOS_KERNEL_SECTIONS(.bss .bss.*)                                   \
        KICKOS_KERNEL_SECTIONS(COMMON)                                        \
        KICKOS_KERNEL_SECTIONS(.kickos_bss_last.*)                            \
        . = ALIGN(4);                                                         \
        _ebss = .;                                                            \
    } > RAM                                                                   \
    KICKOS_CORTEXM_APP_CODE                                                   \
    appdata_head                                                              \
    {                                                                         \
        __kickos_appdata_start = .;                                           \
        KICKOS_APP_SECTIONS(.data .data.*)                                    \
        . = ALIGN(8);                                                         \
        __kickos_appdata_load_end = .;                                        \
    } > RAM AT > FLASH                                                        \
    .appbss (__kickos_appdata_start + SIZEOF(.appdata)) (NOLOAD) : ALIGN(4)   \
    {                                                                         \
        KICKOS_APP_SECTIONS(.bss .bss.* COMMON)                               \
        . = ALIGN(4);                                                         \
        _appdata_used_end = .;                                                \
        . = MAX(., __kickos_appdata_start + _appdata_size);                   \
        __kickos_appdata_end = .;                                             \
        _kickos_heap_start = ALIGN(_appdata_used_end, 8);                     \
        _kickos_heap_limit = __kickos_appdata_end;                            \
    } > RAM                                                                   \
    KICKOS_CORTEXM_APP_WINDOW_SYMBOLS(__kickos_appdata_end)                   \
    KICKOS_CORTEXM_ARENA
#else
#define KICKOS_CORTEXM_DATA(appdata_head)                                     \
    _sidata = LOADADDR(.data);                                                \
    .data : ALIGN(4)                                                          \
    {                                                                         \
        _sdata = .;                                                           \
        KICKOS_KERNEL_SECTIONS(.data .data.*)                                 \
        . = ALIGN(4);                                                         \
        _edata = .;                                                           \
    } > RAM AT > FLASH                                                        \
    .bss (NOLOAD) : ALIGN(4)                                                  \
    {                                                                         \
        _sbss = .;                                                            \
        KICKOS_KERNEL_SECTIONS(.bss .bss.*)                                   \
        KICKOS_KERNEL_SECTIONS(COMMON)                                        \
        KICKOS_KERNEL_SECTIONS(.kickos_bss_last.*)                            \
        . = ALIGN(4);                                                         \
        _ebss = .;                                                            \
    } > RAM                                                                   \
    KICKOS_CORTEXM_APP_CODE                                                   \
    .appdata : ALIGN(8)                                                       \
    {                                                                         \
        KICKOS_APP_SECTIONS(.data .data.*)                                    \
        . = ALIGN(8);                                                         \
    } > RAM AT > FLASH                                                        \
    .appbss (ADDR(.appdata) + SIZEOF(.appdata)) (NOLOAD) : ALIGN(4)           \
    {                                                                         \
        KICKOS_APP_SECTIONS(.bss .bss.* COMMON)                               \
        . = ALIGN(4);                                                         \
        _appdata_used_end = .;                                                \
    } > RAM                                                                   \
    .userheap (NOLOAD) :                                                      \
    {                                                                         \
        . = ALIGN(8);                                                         \
        _kickos_heap_start = .;                                               \
        . += KICKOS_USER_HEAP_SIZE;                                           \
        _kickos_heap_limit = .;                                               \
    } > RAM                                                                   \
    KICKOS_CORTEXM_APP_WINDOW_SYMBOLS(_kickos_heap_limit)                     \
    KICKOS_CORTEXM_ARENA
#endif

#define KICKOS_CORTEXM_DISCARD                                                \
    /DISCARD/ : { *(.ARM.attributes) }                                        \
    KICKOS_NONALLOC_SECTIONS

/* An empty kernel .bss means the kernel's selectors matched nothing, kernel writable state
 * having leaked into the app window; an empty .appdata is legitimate. */
#if KICKOS_IMAGES_PLACED && KICKOS_HAVE_MPU
#define KICKOS_CORTEXM_WINDOW_ASSERTS()                                       \
    ASSERT(_ebss > _sbss,                                                     \
           "KickOS: kernel .bss empty: closed-set archive selector matched nothing (renamed lib?)")
#elif KICKOS_HAVE_MPU
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
    KICKOS_CORTEXM_IMAGES_ASSERTS()                                       \
    KICKOS_APP_IMAGE_SPLIT_NONE()

#endif
