/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * NOT a C header: the GNU ld macros of an RV32IMAC image, which each chip's image template
 * (image.ld, or <chip>.image.ld beside its script) instantiates. KICKOS_RV32_IMAGE_PAD, which a
 * template may define first, is the boundary every part of a code span ends on, and
 * KICKOS_RV32_IMAGE_CODE_REGION the region the span links in, RAM unless the template says.
 */
#ifndef KICKOS_ARCH_RISCV_COMMON_RV32IMAC_IMAGE_LD_H
#define KICKOS_ARCH_RISCV_COMMON_RV32IMAC_IMAGE_LD_H

#include <sections.ld.h>

#ifndef KICKOS_RV32_IMAGE_PAD
#define KICKOS_RV32_IMAGE_PAD 4
#endif
#ifndef KICKOS_RV32_IMAGE_CODE_REGION
#define KICKOS_RV32_IMAGE_CODE_REGION RAM
#endif

/* Image n's code span: header, text and rodata, its DWARF EH ended by a zero-length CIE, its
 * constructor window, then, where `tpl` is 1, the template its window's .data starts from. Under
 * KICKOS_RV32_IMAGE_WINDOW a task restarted or created on request has one, which the kernel fills
 * at boot from the .data the loader places where it runs (kernel/task/image.cc); under
 * KICKOS_RV32_IMAGE_WINDOW_LOADED every image has one, which holds the .data the loader places. */
#define KICKOS_RV32_IMAGE_CODE(n, tpl)                                        \
    .kickos_image##n##_header : ALIGN(4)                                      \
    {                                                                         \
        KICKOS_LD_IMAGE_SYM(n, code_start) = .;                               \
        KEEP(*kickos_image##n.o(.kickos_image_header))                        \
        . = ALIGN(KICKOS_RV32_IMAGE_PAD);                                     \
    } > KICKOS_RV32_IMAGE_CODE_REGION                                         \
    .kickos_image##n##_text : ALIGN(4)                                        \
    {                                                                         \
        *kickos_image##n.o(.text .text.*)                                     \
        *kickos_image##n.o(.rodata .rodata.*)                                 \
        *kickos_image##n.o(.srodata .srodata.*)                               \
        *kickos_image##n.o(.gcc_except_table .gcc_except_table.*)             \
        . = ALIGN(4);                                                         \
        KICKOS_LD_IMAGE_SYM(n, eh_frame) = .;                                 \
        KEEP(*kickos_image##n.o(.eh_frame))                                   \
        LONG(0)                                                               \
        . = ALIGN(KICKOS_RV32_IMAGE_PAD);                                     \
    } > KICKOS_RV32_IMAGE_CODE_REGION                                         \
    .kickos_image##n##_init_array : ALIGN(4)                                  \
    {                                                                         \
        KICKOS_LD_IMAGE_SYM(n, ctor_start) = .;                               \
        KEEP(*kickos_image##n.o(SORT(.init_array.*)))                         \
        KEEP(*kickos_image##n.o(.init_array))                                 \
        KICKOS_LD_IMAGE_SYM(n, ctor_end) = .;                                 \
        . = ALIGN(KICKOS_RV32_IMAGE_PAD);                                     \
    } > KICKOS_RV32_IMAGE_CODE_REGION                                         \
    .kickos_image##n##_dtpl (NOLOAD) : ALIGN(4 + 4 * (tpl))                   \
    {                                                                         \
        . += (tpl) * SIZEOF(.kickos_image##n##_data);                         \
        . = ALIGN(4);                                                         \
    } > KICKOS_RV32_IMAGE_CODE_REGION                                         \
    KICKOS_LD_IMAGE_SYM(n, code_end) = .;

/* Image n's window at `base`, or where RAM has room, aligned to `align`: gp small data and .data,
 * .bss, `heap` more bytes, then the window shaped by KICKOS_IMAGE_WINDOW_END; its heap is the
 * window past .bss. Small data first, so image 1's is within the reach of the gp the chip script
 * anchors at its window. Its .data loads where it runs. */
#define KICKOS_RV32_IMAGE_WINDOW(n, base, align, heap, tpl)                   \
    KICKOS_RV32_IMAGE_WINDOW_AT(n, base, , align, heap, tpl)

/* The same window, its .data loaded at its template in the code span, which tpl 1 reserves. */
#define KICKOS_RV32_IMAGE_WINDOW_LOADED(n, base, align, heap)                 \
    KICKOS_RV32_IMAGE_WINDOW_AT(n, base, AT(ADDR(.kickos_image##n##_dtpl)), align, heap, 1)

#define KICKOS_RV32_IMAGE_WINDOW_AT(n, base, load, align, heap, tpl)          \
    .kickos_image##n##_data base : load ALIGN(align)                          \
    {                                                                         \
        KICKOS_LD_IMAGE_SYM(n, window_start) = .;                             \
        *kickos_image##n.o(.sdata .sdata.* .sdata2.*)                         \
        *kickos_image##n.o(.data .data.*)                                     \
        . = ALIGN(8);                                                         \
    } > RAM                                                                   \
    .kickos_image##n##_bss (KICKOS_LD_IMAGE_SYM(n, window_start)              \
                            + SIZEOF(.kickos_image##n##_data)) (NOLOAD) : ALIGN(4) \
    {                                                                         \
        *kickos_image##n.o(.sbss .sbss.*)                                     \
        *kickos_image##n.o(.bss .bss.* COMMON)                                \
        . = ALIGN(4);                                                         \
        KICKOS_LD_IMAGE_SYM(n, bss_end) = .;                                  \
        . = ALIGN(8);                                                         \
        . += (heap);                                                          \
        KICKOS_IMAGE_WINDOW_END(KICKOS_LD_IMAGE_SYM(n, window_start))         \
        KICKOS_LD_IMAGE_SYM(n, window_end) = .;                               \
    } > RAM                                                                   \
    KICKOS_LD_IMAGE_SYM(n, data_load) = ADDR(.kickos_image##n##_dtpl)        \
        + (1 - (tpl)) * (ADDR(.kickos_image##n##_data) - ADDR(.kickos_image##n##_dtpl)); \
    KICKOS_LD_IMAGE_SYM(n, data_start) = ADDR(.kickos_image##n##_data);       \
    KICKOS_LD_IMAGE_SYM(n, data_end) =                                        \
        ADDR(.kickos_image##n##_data) + SIZEOF(.kickos_image##n##_data);      \
    KICKOS_LD_IMAGE_SYM(n, heap_start) = ALIGN(KICKOS_LD_IMAGE_SYM(n, bss_end), 8); \
    KICKOS_LD_IMAGE_SYM(n, heap_end) =                                        \
        MAX(KICKOS_LD_IMAGE_SYM(n, window_end), KICKOS_LD_IMAGE_SYM(n, heap_start)); \
    KICKOS_IMAGE_WINDOW_SIZE(n)                                               \
    KICKOS_LD_IMAGE_SYM(n, pmp_code_start) = KICKOS_LD_C_SYM(__kickos_pmp_code_start); \
    KICKOS_LD_IMAGE_SYM(n, pmp_code_end) = KICKOS_LD_C_SYM(__kickos_pmp_code_end); \
    KICKOS_IMAGE_NAMES(n)

/* The first window's base: past kernel .bss. */
#define KICKOS_RV32_WINDOWS_BASE KICKOS_LD_C_SYM(_ebss)

#endif
