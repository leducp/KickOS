/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The user side of the syscall trap: arch_syscall(nr, a0, a1, a2, a3), arch_syscall64 the same
 * entry, and arch_syscall_reg where the arch carries the register-form IPC trap. Part of the user
 * runtime, so every image carries its own.
 */

    .section .text.kickos_arch_syscall, "ax", @progbits
    .balign 4

/* arch_syscall(nr, a0, a1, a2, a3). ra survives the trap in the frame. arch_syscall64 is the
   SAME trap: a0:a1 is the psABI's 64-bit pair. */
    .global arch_syscall
    .global arch_syscall64
arch_syscall:
arch_syscall64:
    ecall
    ret

/* int32_t arch_syscall_reg(uint32_t* io): the register-carrying IPC trap. io[0..7] load
   a0-a7; on return io[1..5] take back a1-a5 and a0 is the result. t0 holds `io` across the
   ecall because the trap frame saves and restores it. */
    .global arch_syscall_reg
arch_syscall_reg:
    mv      t0, a0
    lw      a0,  0(t0)
    lw      a1,  4(t0)
    lw      a2,  8(t0)
    lw      a3, 12(t0)
    lw      a4, 16(t0)
    lw      a5, 20(t0)
    lw      a6, 24(t0)
    lw      a7, 28(t0)
    ecall
    sw      a1,  4(t0)
    sw      a2,  8(t0)
    sw      a3, 12(t0)
    sw      a4, 16(t0)
    sw      a5, 20(t0)
    ret
