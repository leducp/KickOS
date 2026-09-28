/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The LP reset PC is 0x50000080. HP copies this vector and jump to LP SRAM
 * before the PMU wake; the LP kernel's own mtvec replaces it after entry.
 */

#if KICKOS_AMP_OWN_IMAGE && KICKOS_AMP_NODE_ID == 0
    .section .rodata.lp_boot, "a", @progbits
    .balign 4
    .global kickos_c6_amp_lp_stub_start
kickos_c6_amp_lp_stub_start:
    .option push
    .option norvc
    .rept 32
    j       lp_early_fault
    .endr
    li      t0, (KICKOS_AMP_PARTITION_BASE + KICKOS_AMP_NODE_SHARE)
    jr      t0
lp_early_fault:
    li      t0, 0x50000204
    csrr    t1, mcause
    sw      t1, 0(t0)
    csrr    t1, mepc
    sw      t1, 4(t0)
    csrr    t1, mtval
    sw      t1, 8(t0)
1:  wfi
    j       1b
    .option pop
    .global kickos_c6_amp_lp_stub_end
kickos_c6_amp_lp_stub_end:
#endif
