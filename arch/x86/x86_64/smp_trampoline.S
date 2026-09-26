/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * SIPI enters at a reserved physical page below 1 MiB. The whole
 * blob is copied there. All references within it are offsets from the copied
 * start; the 64-bit destination, stack, CR3, CR4 bits and GDT base are patched by the BSP.
 * The AP uses the BSP's identity-mapped UEFI root after ExitBootServices, so it takes the
 * BSP's paging depth too: CR4.LA57 cannot change once long mode is active.
 */

    .section .rodata.ap_trampoline, "a"
    .balign 16
    .globl kickos_x86_64_ap_begin
kickos_x86_64_ap_begin:
    .code16
    cli
    cld
    movw    %cs, %ax
    movw    %ax, %ds
    movw    %ax, %ss
    movw    $0xff0, %sp

    lgdtl   .Lgdt_ptr - kickos_x86_64_ap_begin

    movl    %cr4, %eax
    orl     kickos_x86_64_ap_cr4 - kickos_x86_64_ap_begin, %eax
    movl    %eax, %cr4
    movl    kickos_x86_64_ap_cr3 - kickos_x86_64_ap_begin, %eax
    movl    %eax, %cr3

    movl    $0xc0000080, %ecx        /* EFER.LME | NXE */
    rdmsr
    orl     $0x900, %eax
    wrmsr

    movl    %cr0, %eax
    orl     $0x80010001, %eax       /* CR0.PG | CR0.WP | CR0.PE */
    movl    %eax, %cr0
    .byte   0x66, 0xea             /* far jump with 32-bit offset */
    .globl  kickos_x86_64_ap_jump_target
kickos_x86_64_ap_jump_target:
    .long   0                      /* patched to physical .Llong */
    .word   0x08

    .code64
.Llong:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movq    kickos_x86_64_ap_stack(%rip), %rsp
    xorq    %rbp, %rbp
    movq    kickos_x86_64_ap_entry(%rip), %rax
    jmp     *%rax

    .globl kickos_x86_64_ap_long
kickos_x86_64_ap_long = .Llong

    .balign 8
.Lgdt:
    .globl kickos_x86_64_ap_gdt
kickos_x86_64_ap_gdt:
    .quad   0
    .quad   0x00af9a000000ffff     /* 64-bit kernel code, selector 0x08 */
    .quad   0x00cf92000000ffff     /* kernel data, selector 0x10 */
.Lgdt_ptr:
    .word   (.Lgdt_ptr - .Lgdt - 1)
    .globl  kickos_x86_64_ap_gdt_base
kickos_x86_64_ap_gdt_base:
    .long   0

    .balign 8
    .globl kickos_x86_64_ap_cr3
kickos_x86_64_ap_cr3:
    .long   0
    .globl kickos_x86_64_ap_cr4
kickos_x86_64_ap_cr4:
    .long   0
    .globl kickos_x86_64_ap_stack
kickos_x86_64_ap_stack:
    .quad   0
    .globl kickos_x86_64_ap_entry
kickos_x86_64_ap_entry:
    .quad   0
    .globl kickos_x86_64_ap_end
kickos_x86_64_ap_end:
    .code64
