/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * x86_64 context switch, thread entry and the fast syscall entry.
 *
 * The displacements below are literal, and arch_x86_64.cc static_asserts every one of them
 * against struct trap_frame.
 */

#include <kickos/arch/ring3.h>

    .equ KOS_F_R15,    0
    .equ KOS_F_R14,    8
    .equ KOS_F_R13,    16
    .equ KOS_F_R12,    24
    .equ KOS_F_R11,    32
    .equ KOS_F_R10,    40
    .equ KOS_F_R9,     48
    .equ KOS_F_R8,     56
    .equ KOS_F_RBP,    64
    .equ KOS_F_RDI,    72
    .equ KOS_F_RSI,    80
    .equ KOS_F_RDX,    88
    .equ KOS_F_RCX,    96
    .equ KOS_F_RBX,    104
    .equ KOS_F_RAX,    112
    .equ KOS_F_VECTOR, 120
    .equ KOS_F_ERROR,  128
    .equ KOS_F_RIP,    136
    .equ KOS_F_CS,     144
    .equ KOS_F_RFLAGS, 152
    .equ KOS_F_RSP,    160
    .equ KOS_F_SS,     168
    .equ KOS_FRAME_SIZE, 176
    .if (KOS_FRAME_SIZE % 16) != 0
    .error "the frame size must preserve the psABI stack alignment"
    .endif

/* struct arch_context: sp at displacement 0. */
    .equ KOS_CTX_SP,   0

    .equ KOS_CPU_KERNEL_SP, KICKOS_X86_64_CPU_KERNEL_SP
    .equ KOS_CPU_USER_RSP,  KICKOS_X86_64_CPU_USER_RSP

    .section .text, "ax"

/* void kickos_x86_64_switch_now(struct arch_context* from, struct arch_context* to)
 *
 * Builds `from`'s frame around the return address and resumes `to`'s.
 *
 * RFLAGS is captured FIRST, before any instruction here sets a flag: a switch made under the
 * kernel IrqLock captures IF clear, so the thread comes back with interrupts still masked.
 */
    .balign 16
    .globl kickos_x86_64_switch_now
kickos_x86_64_switch_now:
    pushfq
    subq    $KOS_FRAME_SIZE, %rsp
    movq    %rax, KOS_F_RAX(%rsp)
    movq    %rbx, KOS_F_RBX(%rsp)
    movq    %rcx, KOS_F_RCX(%rsp)
    movq    %rdx, KOS_F_RDX(%rsp)
    movq    %rsi, KOS_F_RSI(%rsp)
    movq    %rdi, KOS_F_RDI(%rsp)
    movq    %rbp, KOS_F_RBP(%rsp)
    movq    %r8,  KOS_F_R8(%rsp)
    movq    %r9,  KOS_F_R9(%rsp)
    movq    %r10, KOS_F_R10(%rsp)
    movq    %r11, KOS_F_R11(%rsp)
    movq    %r12, KOS_F_R12(%rsp)
    movq    %r13, KOS_F_R13(%rsp)
    movq    %r14, KOS_F_R14(%rsp)
    movq    %r15, KOS_F_R15(%rsp)

    movq    KOS_FRAME_SIZE(%rsp), %rax          /* the pushfq word */
    movq    %rax, KOS_F_RFLAGS(%rsp)
    movq    (KOS_FRAME_SIZE + 8)(%rsp), %rax      /* the return address */
    movq    %rax, KOS_F_RIP(%rsp)
    /* The stack pointer the resumed thread runs on: past this frame, past the pushfq word
     * and past the return address, all three of which the resume consumes. */
    leaq    (KOS_FRAME_SIZE + 16)(%rsp), %rax
    movq    %rax, KOS_F_RSP(%rsp)
    xorl    %eax, %eax
    movw    %cs, %ax
    movq    %rax, KOS_F_CS(%rsp)
    xorl    %eax, %eax
    movw    %ss, %ax
    movq    %rax, KOS_F_SS(%rsp)
    movq    $0, KOS_F_VECTOR(%rsp)
    movq    $0, KOS_F_ERROR(%rsp)

    movq    %rsp, KOS_CTX_SP(%rdi)
    movq    KOS_CTX_SP(%rsi), %rsp
    jmp     kickos_x86_64_resume

/* void kickos_x86_64_start(struct arch_context* first): enters the first thread. The boot
 * stack is abandoned, which arch.h permits. */
    .balign 16
    .globl kickos_x86_64_start
kickos_x86_64_start:
    movq    KOS_CTX_SP(%rdi), %rsp
    jmp     kickos_x86_64_resume

/* The address arch_context_init leaves at the top of a new thread's stack, so the entry
 * function's own `ret` lands here. The `call` is what leaves the stack 16-byte aligned at
 * kickos_thread_return's entry, as the SysV convention wants. */
    .balign 16
    .globl kickos_x86_64_thread_exit
kickos_x86_64_thread_exit:
    call    kickos_thread_return
.Lexit_dead:
    cli
    hlt
    jmp     .Lexit_dead

/* --- The fast syscall entry ------------------------------------------------
 * Privileged execution begins on the stack pointer the CALLER chose, and the three
 * instructions below are what leave it. NONE of the three may dereference it or push: the
 * swapgs takes no operand, and the two moves address the per-core block through the gs base.
 * They cannot be interrupted either: IA32_FMASK clears the interrupt flag, and the two
 * classes the flag does not hold off have interrupt-stack-table slots of their own (desc.h).
 *
 * The caller is always at ring 3 here, arranged and not checked: arch_syscall below calls the
 * dispatch directly when already privileged, which is what lets the swapgs be unconditional.
 * The kernel block is empty on arrival, a thread reaching ring 3 only through the resume
 * epilogue, so the entry may stand at its top.
 */
    .balign 16
    .globl kickos_x86_64_syscall_entry
kickos_x86_64_syscall_entry:
    swapgs
    movq    %rsp, %gs:KOS_CPU_USER_RSP
    movq    %gs:KOS_CPU_KERNEL_SP, %rsp

/* The frame, in the order kickos_x86_64_resume pops it. cs and ss are stamped: this entry
 * serves exactly one privilege level. */
    pushq   $KICKOS_X86_64_SEL_USER_DATA
    pushq   %gs:KOS_CPU_USER_RSP
    pushq   %r11
    pushq   $KICKOS_X86_64_SEL_USER_CODE
    pushq   %rcx
    pushq   $0
    pushq   $KICKOS_X86_64_VECTOR_SYSCALL
    pushq   %rax
    pushq   %rbx
    pushq   %rcx
    pushq   %rdx
    pushq   %rsi
    pushq   %rdi
    pushq   %rbp
    pushq   %r8
    pushq   %r9
    pushq   %r10
    pushq   %r11
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15

/* The flags the thread comes back with are stated here. The frame is also the resume vehicle
 * for the containment redirect. */
    andq    $KICKOS_X86_64_RFLAGS_USER_KEEP, KOS_F_RFLAGS(%rsp)
    orq     $KICKOS_X86_64_RFLAGS_USER_FORCE, KOS_F_RFLAGS(%rsp)

/* syscall_dispatch(nr, a0, a1, a2, a3). SYSCALL destroys rcx, so arch_syscall moved a2 to
 * r10 first and the frame holds it there. The dispatch runs on this thread's own kernel block
 * in privileged THREAD context, so it may block and be resumed here.
 *
 * rsp IS the frame base and it is 16-byte aligned, so the call needs no adjustment. */
    movq    KOS_F_RDI(%rsp), %rdi
    movq    KOS_F_RSI(%rsp), %rsi
    movq    KOS_F_RDX(%rsp), %rdx
    movq    KOS_F_R10(%rsp), %rcx
    movq    KOS_F_R8(%rsp), %r8
    call    syscall_dispatch
/* Into the frame, because the resume reloads rax from there. */
    movq    %rax, KOS_F_RAX(%rsp)
    jmp     kickos_x86_64_resume

/* arch_syscall(nr, a0, a1, a2, a3), and arch_syscall64 is the same entry.
 *
 * The privilege test is here because SYSCALL records no privilege level; cs is readable at
 * either level and cannot be forged. A privileged caller reaches the dispatch directly.
 */
    .balign 16
    .globl arch_syscall
    .globl arch_syscall64
arch_syscall:
arch_syscall64:
    movw    %cs, %ax
    testb   $3, %al
    je      .Lsyscall_privileged
    movq    %rcx, %r10
    syscall
    ret
.Lsyscall_privileged:
    jmp     syscall_dispatch
