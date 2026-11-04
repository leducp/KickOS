/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * NOT a C header: the GNU ld macros the RX72M script and its image template (rx72m.image.ld)
 * share.
 */
#ifndef KICKOS_ARCH_RX_CHIP_RX72M_RX72M_IMAGE_LD_H
#define KICKOS_ARCH_RX_CHIP_RX72M_RX72M_IMAGE_LD_H

/* sections.ld.h's window macros spell their C names through this, in two levels so that an
   argument is expanded before the paste. */
#define KICKOS_LD_C_SYM_(name) _##name
#define KICKOS_LD_C_SYM(name) KICKOS_LD_C_SYM_(name)

#include <sections.ld.h>

/* An image's text and constant data, `sections` selecting its inputs. */
#define KICKOS_RX_IMAGE_TEXT(sections)                                        \
        sections(.text .text.*)                                               \
        sections(P P_*)                                                       \
        sections(.rodata .rodata.*)                                           \
        sections(C C_1 C_2 C$*)                                               \
        sections(W W_1 W_2)                                                   \
        sections(L)                                                           \
        . = ALIGN(4);                                                         \
        KEEP(sections(.gcc_except_table .gcc_except_table.*))                 \
        . = ALIGN(4);

/* Image n's code span: its header, text and constant data, constructor window, then the room
 * its window's .data loads into. */
#define KICKOS_RX_IMAGE_CODE(n)                                               \
    .kickos_image##n##_header : ALIGN(4)                                      \
    {                                                                         \
        KICKOS_LD_IMAGE_SYM(n, code_start) = .;                               \
        KEEP(*kickos_image##n.o(.kickos_image_header))                        \
    } > ROM                                                                   \
    .kickos_image##n##_text : ALIGN(4)                                        \
    {                                                                         \
        *kickos_image##n.o(.text .text.*)                                     \
        *kickos_image##n.o(P P_*)                                             \
        *kickos_image##n.o(.rodata .rodata.*)                                 \
        *kickos_image##n.o(C C_1 C_2 C$*)                                     \
        *kickos_image##n.o(W W_1 W_2)                                         \
        *kickos_image##n.o(L)                                                 \
        . = ALIGN(4);                                                         \
        KEEP(*kickos_image##n.o(.gcc_except_table .gcc_except_table.*))       \
        . = ALIGN(4);                                                         \
    } > ROM                                                                   \
    .kickos_image##n##_init_array : ALIGN(4)                                  \
    {                                                                         \
        KICKOS_LD_IMAGE_SYM(n, ctor_start) = .;                               \
        KEEP(*kickos_image##n.o(SORT(.init_array.*)))                         \
        KEEP(*kickos_image##n.o(.init_array))                                 \
        KICKOS_LD_IMAGE_SYM(n, ctor_end) = .;                                 \
    } > ROM                                                                   \
    .kickos_image##n##_dload (NOLOAD) : ALIGN(4)                              \
    {                                                                         \
        . += SIZEOF(.kickos_image##n##_data);                                 \
    } > ROM                                                                   \
    KICKOS_LD_IMAGE_SYM(n, code_end) =                                        \
        ADDR(.kickos_image##n##_dload) + SIZEOF(.kickos_image##n##_dload);    \
    KICKOS_LD_IMAGE_SYM(n, eh_frame) = 0;

/* Image n's window at `base`, or where RAM has room, aligned to `align`: .data with its emutls
 * control blocks first, contiguous for user/src/arch/rxv3/emutls.cc to index, loaded from the
 * room at the end of its code span, .bss, `heap` more bytes, then the window shaped by
 * KICKOS_IMAGE_WINDOW_END; its heap is the window past .bss. */
#define KICKOS_RX_IMAGE(n, base, align, heap)                                 \
    .kickos_image##n##_data base : AT(ADDR(.kickos_image##n##_dload)) ALIGN(align) \
    {                                                                         \
        KICKOS_LD_IMAGE_SYM(n, window_start) = .;                             \
        KICKOS_LD_IMAGE_SYM(n, emutls_start) = .;                             \
        *kickos_image##n.o(SORT_BY_NAME(.data.__emutls_v.*))                  \
        KICKOS_LD_IMAGE_SYM(n, emutls_end) = .;                               \
        *kickos_image##n.o(.data .data.*)                                     \
        *kickos_image##n.o(D D_1 D_2)                                         \
        . = ALIGN(8);                                                         \
    } > RAM                                                                   \
    .kickos_image##n##_bss (KICKOS_LD_IMAGE_SYM(n, window_start)              \
                            + SIZEOF(.kickos_image##n##_data)) (NOLOAD) : ALIGN(4) \
    {                                                                         \
        *kickos_image##n.o(.bss .bss.* COMMON)                                \
        *kickos_image##n.o(B B_1 B_2)                                         \
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
#define KICKOS_RX_WINDOWS_BASE MAX(KICKOS_LD_C_SYM(_ebss), _kernel_data_top)

#endif
