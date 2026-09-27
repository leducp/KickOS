/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * M9.9 bring-up witness. The HP copies the stub to LP SRAM and the payload to
 * a separate HP SRAM range. The LP reset PC is 0x50000080; the first 128
 * bytes are its vector table. Only the stub's jump is in LP SRAM.
 */

    .section .rodata.lp_probe, "a", @progbits
    .balign 4
    .global kickos_c6_lp_probe_start
kickos_c6_lp_probe_start:
    .option push
    .option norvc
    .rept 30
    j       lp_fault
    .endr
    j       lp_irq
    j       lp_fault
    li      t0, 0x40840000
    jr      t0
lp_fault:
    li      t0, 0x50000204
    csrr    t1, mcause
    sw      t1, 0(t0)
    csrr    t1, mepc
    sw      t1, 4(t0)
    csrr    t1, mtval
    sw      t1, 8(t0)
2:  wfi
    j       2b
lp_irq:
    li      t0, 0x50000214
    csrr    t1, mcause
    sw      t1, 0(t0)
    li      t0, 0x600B2820
    lw      t1, 0(t0)
    li      t0, 0x50000218
    sw      t1, 0(t0)
    li      t0, 0x600B0170
    lw      t1, 0(t0)
    li      t0, 0x5000021C
    sw      t1, 0(t0)
    li      t0, 0x600B0178
    li      t1, 0x80000000
    sw      t1, 0(t0)
    mret
    .option pop
    .global kickos_c6_lp_probe_end
kickos_c6_lp_probe_end:

    .section .rodata.lp_payload, "a", @progbits
    .balign 4
    .global kickos_c6_lp_payload_start
kickos_c6_lp_payload_start:
    .option push
    .option norvc
    li      t1, 0x4c503939
    li      t0, 0x50000200
    sw      t1, 0(t0)
    .option norelax
    lui     t0, %hi(kickos_c6_lp_marker)
    addi    t0, t0, %lo(kickos_c6_lp_marker)
    sw      t1, 0(t0)
    li      t0, 0x50000210
    sw      t1, 0(t0)
    li      t0, 0x600B0184
    li      t1, 0x40000000
    sw      t1, 0(t0)             /* LP -> HP PMU software trigger */
    li      t1, 0x40000000
    csrs    mie, t1
    li      t1, 0x8
    csrs    mstatus, t1
3:  wfi
    j       3b
    .option pop
    .global kickos_c6_lp_payload_end
kickos_c6_lp_payload_end:
