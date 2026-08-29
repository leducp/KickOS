/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * x86_64 interrupt entry. desc_init fills all 256 interrupt gates from the stub table below.
 * The one resume epilogue serves both this entry and the voluntary switch (switch.S).
 */

/* The vectors that push an error code of their own (Intel SDM Vol 3, Table 6-1): 8, 10, 11,
 * 12, 13, 14, 17, 21, 29, 30. Every other stub pushes a zero in its place. */
    .equ KOS_ERR_MASK, 0x60227d00

    .section .text, "ax"

    .balign 16
    .globl kickos_x86_64_vector_base
kickos_x86_64_vector_base:

    .altmacro
    .macro KOS_STUB vec
    .balign 16
.Lvector_\vec:
/* The mask is only 32 bits wide, and a shift of 64 or more is out of range for the
 * assembler, so the test is nested rather than folded into one condition. */
    .set kos_ec, 0
    .if \vec < 32
    .set kos_ec, (KOS_ERR_MASK >> \vec) & 1
    .endif
    .if kos_ec == 0
    pushq   $0
    .endif
    pushq   $\vec
    jmp     kickos_x86_64_trap_common
    .endm

    .set kos_v, 0
    .rept 256
    KOS_STUB %kos_v
    .set kos_v, kos_v + 1
    .endr

/* Offsets from kickos_x86_64_vector_base, same section, so each folds to a constant: an
 * absolute table would need PE base relocations this image has none of. */
    .macro KOS_OFF vec
    .long   .Lvector_\vec - kickos_x86_64_vector_base
    .endm

    .balign 4
    .globl kickos_x86_64_vector_offsets
kickos_x86_64_vector_offsets:
    .set kos_v, 0
    .rept 256
    KOS_OFF %kos_v
    .set kos_v, kos_v + 1
    .endr
    .noaltmacro

/* On entry: [rsp] vector, [rsp+8] error code, then the five words the hardware pushed.
 * The 15 general registers go on in the order struct trap_frame declares them, r15 last so
 * it lands at offset 0.
 *
 * rsp is aligned DOWN after the frame pointer is taken: the 15 pushes leave it 8 modulo 16,
 * and the reporter is compiled to the SysV convention. The frame stays above the new rsp. */
    .balign 16
    .globl kickos_x86_64_trap_common
kickos_x86_64_trap_common:
/* The kernel's per-core pointer lives in the gs base while ring 0 runs and in
 * IA32_KERNEL_GS_BASE while ring 3 does, so an entry from ring 3 brings it back and an entry
 * from ring 0 must not; the pushed cs is the only record of the interrupted level. It must
 * come FIRST: the displacement below is of the hardware frame plus the stub's two words. */
    testb   $3, 24(%rsp)
    jz      .Ltrap_same_level
    swapgs
.Ltrap_same_level:
/* Delivery leaves the direction flag as the interrupted code set it, and every C body below
 * this call is compiled to the SysV convention. tests/static/check_x86_64_entry_cld.sh reads
 * the assembled object for this instruction. */
    cld
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
    movq    %rsp, %rdi
    andq    $-16, %rsp
    call    kickos_x86_64_trap
    movq    %rax, %rsp

/* The resume epilogue, reached from the entry above and from switch.S. rsp names the base of
 * a struct trap_frame, and the pops mirror the push order above.
 *
 * The 16 bytes dropped are the vector and the error code, which the hardware does not pop. */
    .balign 16
    .globl kickos_x86_64_resume
kickos_x86_64_resume:
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %r11
    popq    %r10
    popq    %r9
    popq    %r8
    popq    %rbp
    popq    %rdi
    popq    %rsi
    popq    %rdx
    popq    %rcx
    popq    %rbx
    popq    %rax
    addq    $16, %rsp
/* The pair of the entry's swap, tested on the FRAME's cs: a switch resumes a frame some other
 * entry built, so what matters is the level this iretq delivers control to. */
    testb   $3, 8(%rsp)
    jz      .Lresume_same_level
    swapgs
.Lresume_same_level:
    iretq

/* SCAFFOLDING: the deliberate faults. Each faulting instruction carries a label of its own,
 * which is what the C side prints the expected instruction pointer from. */

/* void kickos_x86_64_probe_load(uintptr_t addr) */
    .balign 16
    .globl kickos_x86_64_probe_load
    .globl kickos_x86_64_probe_load_site
kickos_x86_64_probe_load:
kickos_x86_64_probe_load_site:
    movq    (%rdi), %rax
    ret

/* void kickos_x86_64_probe_store(uintptr_t addr) */
    .balign 16
    .globl kickos_x86_64_probe_store
    .globl kickos_x86_64_probe_store_site
kickos_x86_64_probe_store:
kickos_x86_64_probe_store_site:
    movq    %rdi, (%rdi)
    ret

/* void kickos_x86_64_probe_fetch(uintptr_t addr): transfers control to addr, so the fault is
 * taken on the INSTRUCTION FETCH and the reported instruction pointer is addr itself. */
    .balign 16
    .globl kickos_x86_64_probe_fetch
kickos_x86_64_probe_fetch:
    jmp     *%rdi

/* void kickos_x86_64_probe_selector(uintptr_t selector): loads a data segment register with
 * a selector past the end of the table, so the fault names it in the error code. The load
 * does not complete, so the segment register keeps the value the report needs. */
    .balign 16
    .globl kickos_x86_64_probe_selector
    .globl kickos_x86_64_probe_selector_site
kickos_x86_64_probe_selector:
kickos_x86_64_probe_selector_site:
    movw    %di, %ds
    ret

/* void kickos_x86_64_probe_ud(void) */
    .balign 16
    .globl kickos_x86_64_probe_ud
    .globl kickos_x86_64_probe_ud_site
kickos_x86_64_probe_ud:
kickos_x86_64_probe_ud_site:
    ud2
    ret

/* void kickos_x86_64_probe_de(void): 1 divided by zero. */
    .balign 16
    .globl kickos_x86_64_probe_de
    .globl kickos_x86_64_probe_de_site
kickos_x86_64_probe_de:
    xorl    %edx, %edx
    movl    $1, %eax
kickos_x86_64_probe_de_site:
    divl    %edx
    ret

/* void kickos_x86_64_probe_soft(void): a software interrupt above the defined vectors.
 * A software interrupt is a TRAP, so the reported instruction pointer is the next
 * instruction and not the `int` itself. */
    .balign 16
    .globl kickos_x86_64_probe_soft
    .globl kickos_x86_64_probe_soft_site
kickos_x86_64_probe_soft:
    int     $0x40
kickos_x86_64_probe_soft_site:
    ret

/* void kickos_x86_64_probe_df(uintptr_t bad_sp): faults with an unusable stack pointer, so
 * delivering the fault faults again and the processor escalates to a double fault. */
    .balign 16
    .globl kickos_x86_64_probe_df
kickos_x86_64_probe_df:
    movq    %rdi, %rsp
    ud2
