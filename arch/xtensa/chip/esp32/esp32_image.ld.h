/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * NOT a C header: the GNU ld macros of an ESP32 image, which its image template
 * (esp32.image.ld) instantiates: its text in FLASHTEXT, what the ROM loads of it in DRAM, and its
 * window in DRAM1.
 */
#ifndef KICKOS_ARCH_XTENSA_CHIP_ESP32_ESP32_IMAGE_LD_H
#define KICKOS_ARCH_XTENSA_CHIP_ESP32_ESP32_IMAGE_LD_H

#include <sections.ld.h>

#define KICKOS_ESP32_IMAGE_TEXT_REGION FLASHTEXT
#define KICKOS_ESP32_IMAGE_WINDOW_REGION DRAM1

/* Image n's text, its code span. */
#define KICKOS_ESP32_IMAGE_TEXT(n)                                            \
    .kickos_image##n##_text : ALIGN(4)                                        \
    {                                                                         \
        KICKOS_LD_IMAGE_SYM(n, code_start) = .;                               \
        *kickos_image##n.o(.literal .literal.*)                               \
        *kickos_image##n.o(.text .text.*)                                     \
        . = ALIGN(4);                                                         \
        KICKOS_LD_IMAGE_SYM(n, code_end) = .;                                 \
    } > KICKOS_ESP32_IMAGE_TEXT_REGION

/* What the ROM loads of image n, in DRAM: its header, rodata and DWARF EH ended by a zero-length
 * CIE, its constructor window, then the template its window's .data starts from. Each part ends
 * on 8 bytes, so with no input section aligned beyond that every byte loaded meets the next and
 * esptool makes every image's loaded part one segment. */
#define KICKOS_ESP32_IMAGE(n)                                                 \
    .kickos_image##n##_header : ALIGN(8)                                      \
    {                                                                         \
        KEEP(*kickos_image##n.o(.kickos_image_header))                        \
        . = ALIGN(8);                                                         \
    } > DRAM                                                                  \
    .kickos_image##n##_rodata : ALIGN(8)                                      \
    {                                                                         \
        *kickos_image##n.o(.rodata .rodata.*)                                 \
        *kickos_image##n.o(.rodata_wlog .rodata_wlog.*)                       \
        KEEP(*kickos_image##n.o(.gcc_except_table .gcc_except_table.*))       \
        . = ALIGN(4);                                                         \
        KICKOS_LD_IMAGE_SYM(n, eh_frame) = .;                                 \
        KEEP(*kickos_image##n.o(.eh_frame))                                   \
        LONG(0)                                                               \
        . = ALIGN(8);                                                         \
    } > DRAM                                                                  \
    .kickos_image##n##_init_array : ALIGN(8)                                  \
    {                                                                         \
        KICKOS_LD_IMAGE_SYM(n, ctor_start) = .;                               \
        KEEP(*kickos_image##n.o(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*))) \
        KEEP(*kickos_image##n.o(.init_array .ctors))                          \
        KICKOS_LD_IMAGE_SYM(n, ctor_end) = .;                                 \
        . = ALIGN(8);                                                         \
    } > DRAM                                                                  \
    .kickos_image##n##_dtpl (NOLOAD) : ALIGN(8)                               \
    {                                                                         \
        . += SIZEOF(.kickos_image##n##_data);                                 \
    } > DRAM

/* Image n's window at `base`, or where DRAM1 has room: .data, loaded at its template and copied in
 * at every start, .bss, `heap` more bytes, 8-aligned from a base aligned to `align`. The ROM loads
 * nothing here. */
#define KICKOS_ESP32_WINDOW(n, base, align, heap)                             \
    .kickos_image##n##_data base : AT(ADDR(.kickos_image##n##_dtpl)) ALIGN(align) \
    {                                                                         \
        KICKOS_LD_IMAGE_SYM(n, window_start) = .;                             \
        *kickos_image##n.o(.data .data.*)                                     \
        . = ALIGN(8);                                                         \
    } > KICKOS_ESP32_IMAGE_WINDOW_REGION                                      \
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
    } > KICKOS_ESP32_IMAGE_WINDOW_REGION                                      \
    KICKOS_LD_IMAGE_SYM(n, data_load) = LOADADDR(.kickos_image##n##_data);    \
    KICKOS_LD_IMAGE_SYM(n, data_start) = ADDR(.kickos_image##n##_data);       \
    KICKOS_LD_IMAGE_SYM(n, data_end) =                                        \
        ADDR(.kickos_image##n##_data) + SIZEOF(.kickos_image##n##_data);      \
    KICKOS_LD_IMAGE_SYM(n, heap_start) = ALIGN(KICKOS_LD_IMAGE_SYM(n, bss_end), 8); \
    KICKOS_LD_IMAGE_SYM(n, heap_end) =                                        \
        MAX(KICKOS_LD_IMAGE_SYM(n, window_end), KICKOS_LD_IMAGE_SYM(n, heap_start)); \
    KICKOS_IMAGE_WINDOW_SIZE(n)                                               \
    KICKOS_IMAGE_NAMES(n)

/* No window is pinned: nothing protects one here. */
#define KICKOS_ESP32_WINDOWS_BASE

#endif
