/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The user side of the syscall trap: arch_syscall(nr, a0, a1, a2, a3), arch_syscall64 the same
 * entry, and arch_syscall_reg where the arch carries the register-form IPC trap. Part of the user
 * runtime, so every image carries its own.
 */

/* No privileged thread runs app code where the board translates (docs/design-m6-mmu.md), so it
 * carries no privileged branch.
 */
    .section .text.kickos_arch_syscall, "ax", @progbits
    .balign 16
    .globl arch_syscall
    .globl arch_syscall64
arch_syscall:
arch_syscall64:
    movq    %rcx, %r10
    syscall
    ret
