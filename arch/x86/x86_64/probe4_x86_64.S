/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * SCAFFOLDING: the SYSRET arm. Nothing else in the image executes a SYSRET.
 */

    .section .text, "ax"

/* void kickos_x86_64_probe_sysret(uintptr_t user_rip, uintptr_t user_rsp, uintptr_t nr)
 *
 * SYSRETQ takes the instruction pointer from rcx and the flags from r11, and leaves rsp as it
 * was, so the caller hands the ring 3 stack in and this seats it. Never returns.
 */
    .balign 16
    .globl kickos_x86_64_probe_sysret
kickos_x86_64_probe_sysret:
    movq    %rdi, %rcx
    movq    $0x202, %r11            /* interrupt flag, plus the bit that always reads one */
    movq    %rsi, %rsp
    swapgs
    sysretq

/* The ring 3 stub SYSRETQ lands on; it reports through the syscall trap, the console being
 * out of reach at ring 3.
 *
 * rdx survives SYSRETQ and carries the call number; rcx and r11 do not.
 * The entry's argument registers are nr, a0, a1, a2, a3 in rdi, rsi, rdx, r10, r8.
 */
    .balign 16
    .globl kickos_x86_64_probe_sysret_stub
kickos_x86_64_probe_sysret_stub:
    movq    %rdx, %rdi
    movw    %cs, %ax
    movzwl  %ax, %esi
    movw    %ss, %ax
    movzwl  %ax, %edx
    xorl    %r10d, %r10d
    xorl    %r8d, %r8d
    syscall
.Lsysret_stuck:
    jmp     .Lsysret_stuck

/* void kickos_x86_64_probe_cr0(void). Three bytes, so the ring 0 control arm can step past
 * it. */
    .balign 16
    .globl kickos_x86_64_probe_cr0
kickos_x86_64_probe_cr0:
    movq    %cr0, %rax
    ret

/* void kickos_x86_64_probe_outb(void): a port write, refused at ring 3 by the task-state
 * segment's absent permission bitmap. */
    .balign 16
    .globl kickos_x86_64_probe_outb
kickos_x86_64_probe_outb:
    movl    $0x80, %edx
    xorl    %eax, %eax
    outb    %al, %dx
    ret

/* void kickos_x86_64_probe_ud2(void). Two bytes, which the reporter steps past. */
    .balign 16
    .globl kickos_x86_64_probe_ud2
kickos_x86_64_probe_ud2:
    ud2
    ret

/* uint64_t kickos_x86_64_probe_flag_forge(uintptr_t nr, uint64_t forged_flags)
 *
 * A ring 3 thread trying to CHOOSE the flags it comes back with, by putting an I/O privilege
 * level of 3 in r11 before the instruction. Returns the flags it actually came back holding.
 *
 * The entry's argument registers are nr, a0, a1, a2, a3 in rdi, rsi, rdx, r10, r8; rdi already
 * holds nr, and rsi is consumed as the forged value.
 */
    .balign 16
    .globl kickos_x86_64_probe_flag_forge
kickos_x86_64_probe_flag_forge:
    movq    %rsi, %r11
    xorl    %esi, %esi
    xorl    %edx, %edx
    xorl    %r10d, %r10d
    xorl    %r8d, %r8d
    syscall
    pushfq
    popq    %rax
    ret
