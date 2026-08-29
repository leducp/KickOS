/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The RV64IMAC supervisor-mode confirmation. The trap vector itself, the frame it builds and
 * the demux under it are in switch.S.
 */

    .section .text, "ax"

/* int kickos_rv64_privilege_probe(void): 1 when the hart is in supervisor mode, 0 when it is
 * still in machine mode.
 *
 * Current privilege is not readable on RISC-V, so the answer comes from a read that must be
 * REFUSED. mstatus is a machine CSR, so csrr on it is an illegal instruction from S-mode, and
 * startup.S delegates that exception (medeleg bit 2) to stvec.
 *
 * THE CALLER'S stvec IS SAVED AND PUT BACK rather than reinstalled by name: this probe must
 * not be a second writer of the vector, or defeating the install in kickos_rv64_init would
 * leave the image working. t2 carries it across the trap because the hardware saves no
 * register and the landing pad writes only a0.
 *
 * THE TRAP LEG CLEARS sstatus.SIE AND NEVER sret's IT BACK, so this must run before any
 * interrupt is enabled.
 *
 * With the delegation defeated as well as the drop, the illegal instruction goes to an mtvec
 * nothing wrote and the hart spins: this probe can fail to answer, but it cannot answer
 * supervisor for a hart in machine mode.
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
