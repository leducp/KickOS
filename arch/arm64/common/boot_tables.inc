/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The two boot roots and their level-2 tables, emitted where a chip's startup.S includes this,
 * after its RAM_L2_BLOCKS and, for an AMP node, its MAP_* partition bounds.
 *
 * THE TABLES ARE IDENTITY-LINKED (the chip script places .rodata.mmu_ident in .text.init):
 * a descriptor names its output by physical address, so every expression here has to be one.
 *
 * Whole and constant, both roots: entry 0 covers the device gigabyte that holds the GIC and the
 * console UART, entry 1 the DRAM this image links into. Every other entry is invalid, so a stray
 * access outside the two faults rather than reading a neighbour. TTBR0's is replaced by a
 * per-task root once a domain owns an address space; TTBR1's stays a constant.
 */

#ifndef KICKOS_ARCH_ARM64_COMMON_BOOT_TABLES_INC
#define KICKOS_ARCH_ARM64_COMMON_BOOT_TABLES_INC

#ifndef MAP_BASE
#define MAP_BASE        __kickos_dram_base
#define MAP_L2_BLOCKS   RAM_L2_BLOCKS
#define OWN_L2_LO       0
#define OWN_L2_HI       RAM_L2_BLOCKS
#define SHARED_L2_LO    0
#define SHARED_L2_HI    0
#define SHARE_NC_L2_LO  0
#endif

/* The one block the kernel is fetched from, read-only and executable at EL1. */
#define CODE_L2_BLOCK   OWN_L2_LO

/* One level-2 table for 0x4000_0000..0x8000_0000, emitted into both roots. */
.macro  L2_PARTITION_TABLE
    .set    _blk, 0
    .rept   MAP_L2_BLOCKS
    .if     _blk == CODE_L2_BLOCK
    .quad   MAP_BASE + (_blk * L2_BLOCK_SIZE) + BLOCK + AF + ATTR_NORMAL + SH_INNER + \
            AP_RO_EL1 + UXN
    .elseif _blk >= SHARE_NC_L2_LO && _blk < SHARED_L2_HI
    .quad   MAP_BASE + (_blk * L2_BLOCK_SIZE) + BLOCK + AF + ATTR_NOCACHE + \
            AP_RW_EL1 + UXN + PXN
    .elseif (_blk >= OWN_L2_LO && _blk < OWN_L2_HI) || \
            (_blk >= SHARED_L2_LO && _blk < SHARED_L2_HI)
    .quad   MAP_BASE + (_blk * L2_BLOCK_SIZE) + BLOCK + AF + ATTR_NORMAL + SH_INNER + \
            AP_RW_EL1 + UXN + PXN
    .else
    .quad   0
    .endif
    .set    _blk, _blk + 1
    .endr
    /* Past the mapped span: invalid, so a stray access faults rather than aliasing. */
    .fill   L2_ENTRIES - MAP_L2_BLOCKS, 8, 0
.endm

    .section .rodata.mmu_ident, "a"
    .balign 4096
    .global kickos_arm64_ident_table
kickos_arm64_ident_table:
    /* EL1-only: every device touch on this port goes through a syscall. */
    .quad   BLOCK + AF + ATTR_DEVICE + UXN + PXN    /* 0x0000_0000..0x4000_0000 */
    /* A table rather than a block, for the split above. DRAM_L1_ENTRY names this slot. */
    .quad   kickos_arm64_ram_table + TABLE
    .fill   L2_ENTRIES - DRAM_L1_ENTRY - 1, 8, 0

/* The high half, VA = PA + __kickos_arm64_va_base: the kernel window and the map of all
 * physical RAM at once, so the level-1 indices are the ones above. Every address space shares it.
 */
    .balign 4096
    .global kickos_arm64_kernel_table
kickos_arm64_kernel_table:
    .quad   BLOCK + AF + ATTR_DEVICE + UXN + PXN
    .quad   kickos_arm64_kernel_ram_table + TABLE
    .fill   L2_ENTRIES - DRAM_L1_ENTRY - 1, 8, 0

/* Level 2 for 0x4000_0000..0x8000_0000 through TTBR0's boot root, one entry per 2 MiB. */
    .balign 4096
    .global kickos_arm64_ram_table
kickos_arm64_ram_table:
    /* EL1-only: nothing runs at EL0 under this root. */
    L2_PARTITION_TABLE

/* The same DRAM through TTBR1: no EL0 access and no EL0 fetch. PXN is clear on the code block
 * alone.
 */
    .balign 4096
    .global kickos_arm64_kernel_ram_table
kickos_arm64_kernel_ram_table:
    L2_PARTITION_TABLE

#endif
