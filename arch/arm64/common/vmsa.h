/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * VMSAv8-64 constants every arm64 startup writes: the level-1 and level-2 descriptor fields,
 * and the three registers that turn the walk on. Architectural, so a chip states none of them;
 * what a chip states beside them is the extent of its own DRAM.
 *
 * Included from startup assembly, so nothing here may be more than a #define.
 */

#ifndef KICKOS_ARCH_ARM64_COMMON_VMSA_H
#define KICKOS_ARCH_ARM64_COMMON_VMSA_H

/* Level-1 block descriptor (4 KiB granule, so one block spans 1 GiB). */
#define BLOCK           (1 << 0)        /* valid, and a block rather than a table */
#define AF              (1 << 10)       /* access flag: 0 faults on first touch */
#define SH_INNER        (3 << 8)        /* inner shareable, for Normal memory */
#define ATTR_NORMAL     (0 << 2)        /* MAIR_EL1 Attr0 */
#define ATTR_DEVICE     (1 << 2)        /* MAIR_EL1 Attr1 */
#define TABLE           (3 << 0)        /* valid, and a table rather than a block */
#define UXN             (1 << 54)       /* never executable at EL0 */
#define PXN             (1 << 53)       /* never executable at EL1 */
#define AP_RW_EL0       (1 << 6)        /* AP=0b01: read/write at EL1 and at EL0 */
#define AP_RO_EL0       (3 << 6)        /* AP=0b11: read-only at EL1 and at EL0 */
#define AP_RW_EL1       (0 << 6)        /* AP=0b00: read/write at EL1, no access at EL0 */
#define AP_RO_EL1       (2 << 6)        /* AP=0b10: read-only at EL1, no access at EL0 */

/* THE TWO ROOTS DESCRIBE THE SAME DRAM WITH DIFFERENT PERMISSIONS, so each carries its own
 * level-2 table. TTBR1's grants EL0 nothing: an unprivileged thread reaches what its own
 * TTBR0 root maps and nothing else, which is what makes the high half the kernel's alone.
 * TTBR0's boot table keeps the EL0 grants the identity map was built with; a per-process
 * root replaces it before any unprivileged thread runs.
 *
 * THE CODE/DATA SPLIT IS FORCED. A region writable at EL0 is not executable at EL1, so
 * giving all of DRAM AP=0b01 for EL0 stacks makes the kernel's next instruction fetch a
 * permission fault (EC 0x21, IFSC 0x0D). One flat block cannot serve both levels, which
 * makes this a W^X split by arithmetic rather than by policy.
 *
 * 2 MiB level-2 blocks are the granularity, so the linker pads the code to that boundary
 * and ASSERTS the fit; a 4 KiB boundary would need a third level filled at runtime.
 */
#define L2_BLOCK_SIZE   0x200000

/* MAIR_EL1: Attr0 Normal write-back read/write-allocate, Attr1 Device-nGnRnE, Attr2 Normal
 * outer and inner non-cacheable (DDI 0487 M.b section D24.2.126: oooo and iiii both 0b0100,
 * and neither half may be 0b0000, which is what would make the attribute Device instead).
 * All three slots are filled here so arch_aspace_memtype_support can honour every type
 * rather than refusing one; a quietly downgraded type is a cacheable view of a DMA buffer.
 */
#define MAIR_VALUE      0x4400FF

/* TCR_EL1. T0SZ and T1SZ are both 25, so each half is a 39-bit VA whose walk starts at
 * level 1 and reaches the blocks below. IPS 0b010 is the A53's 40-bit output.
 *
 * TG1's 4 KiB encoding is 0b10 and TG0's is 0b00 (DDI 0487 M.b, TCR_EL1): the two granule
 * fields do NOT share an encoding, so TG0's constant is the wrong value here.
 */
#define TCR_T0SZ        25
#define TCR_IRGN0_WB    (1 << 8)
#define TCR_ORGN0_WB    (1 << 10)
#define TCR_SH0_INNER   (3 << 12)
#define TCR_TG0_4K      (0 << 14)
#define TCR_T1SZ        (25 << 16)
#define TCR_IRGN1_WB    (1 << 24)
#define TCR_ORGN1_WB    (1 << 26)
#define TCR_SH1_INNER   (3 << 28)
#define TCR_TG1_4K      (2 << 30)
#define TCR_IPS_40BIT   (2 << 32)
#define TCR_VALUE       (TCR_T0SZ | TCR_IRGN0_WB | TCR_ORGN0_WB | TCR_SH0_INNER | \
                         TCR_TG0_4K | TCR_T1SZ | TCR_IRGN1_WB | TCR_ORGN1_WB | \
                         TCR_SH1_INNER | TCR_TG1_4K | TCR_IPS_40BIT)

#define SCTLR_M         (1 << 0)
#define SCTLR_C         (1 << 2)
#define SCTLR_I         (1 << 12)

#endif
