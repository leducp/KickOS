/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 */

    .section .text, "ax"

/* int kickos_rv64_privilege_probe(void): 1 when the hart is in supervisor mode, 0 when it is
 * still in machine mode.
 *
 * Current privilege is not readable on RISC-V, so the answer comes from a read that must be
 * REFUSED. mstatus is a machine CSR, so csrr on it is an illegal instruction from S-mode, and
 * startup.S delegates that exception (medeleg bit 2) to stvec. t2 carries the caller's stvec
 * across the trap: the hardware saves no register and the landing pad writes only a0.
 *
 * THE TRAP LEG CLEARS sstatus.SIE AND NEVER sret's IT BACK, so this must run before any
 * interrupt is enabled.
 */
    .balign 4
    .global kickos_rv64_privilege_probe
kickos_rv64_privilege_probe:
    la      t2, .Lprobe_trap
    csrrw   t2, stvec, t2
    li      a0, 0
    csrr    t1, mstatus
    j       .Lprobe_done
    .balign 4
.Lprobe_trap:
    li      a0, 1
.Lprobe_done:
    csrw    stvec, t2
    ret
