/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The paging mode, and everything the level count is derived from.
 *
 * Pure integer macros: also included from startup.S.
 */
#ifndef KICKOS_ARCH_RV64_PAGING_H
#define KICKOS_ARCH_RV64_PAGING_H

#if defined(__has_include) && __has_include(<kickos/board_config.h>)
#include <kickos/board_config.h>
#endif

#ifndef KICKOS_RV64_SATP_MODE
#error "no KICKOS_RV64_SATP_MODE: the paging mode is the Kconfig choice RV64_PAGING_*, and nothing here may default it"
#endif

/* satp.MODE at XLEN 64 (RISC-V Privileged ISA, satp). The architecture assigns 8 to Sv39, 9 to
 * Sv48 and 10 to Sv57, one 9-bit translation level per step, so the depth is arithmetic on the
 * mode.
 */
#define KICKOS_RV64_SATP_MODE_SHIFT 60
#define KICKOS_RV64_PAGE_LEVELS     (KICKOS_RV64_SATP_MODE - 5)

#if KICKOS_RV64_PAGE_LEVELS < 3 || KICKOS_RV64_PAGE_LEVELS > 4
#error "KICKOS_RV64_SATP_MODE names a mode no run witnesses: 8 (Sv39) and 9 (Sv48) are what the board is measured on"
#endif

/* The leaf is level 0 and the root is the deepest level. */
#define KICKOS_RV64_LEVEL_LEAF 0
#define KICKOS_RV64_LEVEL_ROOT (KICKOS_RV64_PAGE_LEVELS - 1)

/* 9 index bits over a 4 KiB granule, in every mode on this ladder. */
#define KICKOS_RV64_INDEX_BITS    9
#define KICKOS_RV64_GRANULE_SHIFT 12

/* The virtual address width the mode translates: the granule's offset bits plus 9 per level. */
#define KICKOS_RV64_VA_BITS \
    (KICKOS_RV64_GRANULE_SHIFT + KICKOS_RV64_PAGE_LEVELS * KICKOS_RV64_INDEX_BITS)

#define KICKOS_RV64_VPN(va, level) \
    (((va) >> (KICKOS_RV64_GRANULE_SHIFT + (level) * KICKOS_RV64_INDEX_BITS)) & 0x1ff)

#endif
