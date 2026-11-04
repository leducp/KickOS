/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 */

    .section .text.kickos_user_rdseed_present, "ax", @progbits

/* bool kickos_user_rdseed_present(void): CPUID leaf 7 subleaf 0, EBX bit 18. */
    .globl  kickos_user_rdseed_present
    .type   kickos_user_rdseed_present, @function
    .balign 16
kickos_user_rdseed_present:
    pushq   %rbx
    xorl    %eax, %eax
    xorl    %ecx, %ecx
    cpuid
    cmpl    $7, %eax
    jb      1f
    movl    $7, %eax
    xorl    %ecx, %ecx
    cpuid
    xorl    %eax, %eax
    btl     $18, %ebx
    setc    %al
    popq    %rbx
    ret
1:
    xorl    %eax, %eax
    popq    %rbx
    ret
    .size kickos_user_rdseed_present, . - kickos_user_rdseed_present

    .section .text.kickos_user_rdseed, "ax", @progbits

/* bool kickos_user_rdseed(uint64_t* word). The 64-bit form: Zen 5's 16- and 32-bit forms can
 * report a zero seed as ready (AMD-SB-7055). */
    .globl  kickos_user_rdseed
    .type   kickos_user_rdseed, @function
    .balign 16
kickos_user_rdseed:
    rdseed  %rdx
    setc    %al
    movzbl  %al, %eax
    movq    %rdx, (%rdi)
    ret
    .size kickos_user_rdseed, . - kickos_user_rdseed
