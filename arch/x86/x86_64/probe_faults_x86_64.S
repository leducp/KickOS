/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The X2 images' deliberate faults. Each faulting instruction carries a label of its own,
 * which is what probe_x86_64.cc prints the expected instruction pointer from. */

    .section .text, "ax"

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
