/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * VMSAv8-64 descriptor and control-register constants for ARM64 startup.
 * Chips define their DRAM extents separately. Assembly includes this file;
 * use preprocessor definitions only.
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

/* TTBR1 maps DRAM without EL0 access; TTBR0 uses a separate level-2 table.
 * The boot TTBR0 identity map permits EL0 access and is replaced before
 * userspace runs. EL0-writable memory cannot execute at EL1, so code and
 * data need separate blocks. The linker aligns and checks code against
 * the 2 MiB block size; 4 KiB pages would need a runtime third-level table.
 */
#define L2_BLOCK_SIZE   0x200000

/* MAIR_EL1: Attr0 Normal write-back, read/write-allocate; Attr1 Device-nGnRnE;
 * Attr2 Normal inner/outer noncacheable. For Attr2, both nibbles are 0b0100;
 * zero would select Device memory (DDI 0487 M.b, D24.2.126).
 */
#define MAIR_VALUE      0x4400FF

/* T0SZ/T1SZ=25 select 39-bit virtual addresses starting at level 1.
 * IPS=0b010 selects 40-bit outputs. The 4 KiB granule encodings differ:
 * TG1=0b10, TG0=0b00 (DDI 0487 M.b, TCR_EL1).
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
/* Request 16-bit ASIDs. AS is RES0 on 8-bit hardware; read TCR and ASIDBits
 * to determine the effective width. TTBR0 readback cannot determine it.
 * Set this at boot so secondary PEs inherit it through their boot record.
 */
#define TCR_AS_16BIT    (1 << 36)
#define TCR_VALUE       (TCR_T0SZ | TCR_IRGN0_WB | TCR_ORGN0_WB | TCR_SH0_INNER | \
                         TCR_TG0_4K | TCR_T1SZ | TCR_IRGN1_WB | TCR_ORGN1_WB | \
                         TCR_SH1_INNER | TCR_TG1_4K | TCR_IPS_40BIT | TCR_AS_16BIT)

#define SCTLR_M         (1 << 0)
#define SCTLR_C         (1 << 2)
#define SCTLR_I         (1 << 12)

#endif
