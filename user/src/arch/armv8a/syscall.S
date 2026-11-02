/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The user side of the syscall trap: arch_syscall(nr, a0, a1, a2, a3), arch_syscall64 the same
 * entry, and arch_syscall_reg where the arch carries the register-form IPC trap. Part of the user
 * runtime, so every image carries its own.
 */

    .section .text.kickos_arch_syscall, "ax"
    .global arch_syscall
    .global arch_syscall64
arch_syscall:
arch_syscall64:
    svc     #0
    ret
