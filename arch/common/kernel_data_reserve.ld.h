/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * NOT a C header: these macros expand to GNU ld statements. Included from every enforcing
 * chip linker script, which arch/CMakeLists.txt runs through cpp before the link.
 */
#ifndef KICKOS_ARCH_COMMON_KERNEL_DATA_RESERVE_LD_H
#define KICKOS_ARCH_COMMON_KERNEL_DATA_RESERVE_LD_H

/* The images' windows start above kernel .bss and a declared reserve:
 * _kernel_data_top = <kernel data base> + _kernel_data_reserve, so they stop moving with kernel
 * .bss. A chip states its reserve through a KICKOS_KERNEL_DATA_RESERVE default a preset can
 * raise.
 */

/* Invoked where the base is in scope: at file scope on a chip whose kernel .data starts at
 * ORIGIN(RAM), inside SECTIONS on one whose base is a symbol defined there. */
#define KICKOS_KERNEL_DATA_RESERVE_DECL(base)              \
    _kernel_data_reserve = KICKOS_KERNEL_DATA_RESERVE;     \
    _kernel_data_top = (base) + _kernel_data_reserve;

#endif
