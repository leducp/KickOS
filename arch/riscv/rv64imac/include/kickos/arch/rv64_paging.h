/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The paging mode, and everything the level count is derived from. Read by the map editor
 * (arch/riscv/rv64imac/aspace_rv64imac.cc) and by the chip's pre-translation prologue
 * (arch/riscv/chip/virt_rv64/startup.S), which are the only two places a depth exists, so a
 * mode change is one Kconfig line rather than an edit on both sides of the chip<->arch seam
 * (docs/design-m6-mmu.md R3).
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

/* satp.MODE at XLEN 64 (RISC-V Privileged ISA, satp), and the FIELD'S OWN NUMBERING is where
 * the level count comes from: the architecture assigns 8 to Sv39, 9 to Sv48 and 10 to Sv57,
 * one 9-bit translation level per step. So the depth is arithmetic on the mode rather than a
 * second constant beside it. Sv32 is mode 1 at XLEN 32 and is not on this ladder, which is
 * why the range below refuses a mode outright instead of computing a depth for it.
 */
#define KICKOS_RV64_SATP_MODE_SHIFT 60
#define KICKOS_RV64_PAGE_LEVELS     (KICKOS_RV64_SATP_MODE - 5)

#if KICKOS_RV64_PAGE_LEVELS < 3 || KICKOS_RV64_PAGE_LEVELS > 4
#error "KICKOS_RV64_SATP_MODE names a mode no run witnesses: 8 (Sv39) and 9 (Sv48) are what the board is measured on"
#endif

/* LEVELS RUN THE OTHER WAY FROM A64's NUMBERING: the leaf is level 0 and the root is the
 * deepest level. Every walk in the backend and every table index in the prologue is written
 * over these two.
 */
#define KICKOS_RV64_LEVEL_LEAF 0
#define KICKOS_RV64_LEVEL_ROOT (KICKOS_RV64_PAGE_LEVELS - 1)

/* 9 index bits over a 4 KiB granule, in every mode on this ladder. The backend asserts the
 * pair against the entry width its own table type gives.
 */
#define KICKOS_RV64_INDEX_BITS    9
#define KICKOS_RV64_GRANULE_SHIFT 12

/* The virtual address width the mode translates: the granule's offset bits plus 9 per level.
 * The bit below it is the sign bit that splits the space into the halves a space may map and
 * the kernel's window.
 */
#define KICKOS_RV64_VA_BITS \
    (KICKOS_RV64_GRANULE_SHIFT + KICKOS_RV64_PAGE_LEVELS * KICKOS_RV64_INDEX_BITS)

/* The physical range the mode publishes, 56 bits for every RV64 mode. Sv32's 34 is the figure
 * that differs and it is not reachable from here. The architecture publishes no register
 * reporting it, so the mode read back out of satp is what stands behind this number.
 */
#define KICKOS_RV64_PA_BITS 56

/* The table index a virtual address takes at one level. */
#define KICKOS_RV64_VPN(va, level) \
    (((va) >> (KICKOS_RV64_GRANULE_SHIFT + (level) * KICKOS_RV64_INDEX_BITS)) & 0x1ff)

#endif
