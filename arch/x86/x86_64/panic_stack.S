/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * A TRANSLATION UNIT OF ITS OWN, and that is the point: the UEFI probe images link
 * libkickos_arch_x86_64.a WITHOUT the kernel, so a member they already pull for another symbol
 * may carry no reference to one. Both of the symbols below are the kernel's.
 */
/* kickos_panic_stack_enter(msg, file, line, top): leave the interrupted stack for the
 * reporter's own (arch.h). rdi, rsi and edx pass through untouched; rcx is the seat.
 *
 * IF cleared before the move, or an interrupt taken between the two lands on the stack this
 * exists to leave. The `pushq $0` is the psABI alignment a `jmp` entry would otherwise miss:
 * a function expects (rsp + 8) 16-byte aligned, which is what a `call` leaves, and it doubles
 * as a null return address that terminates a backtrace. `jmp` and not `call`: the
 * -fcallgraph-info walk stops at a body with no call edge, which is what keeps the console
 * tail off every measured chain.
 */
#if KICKOS_KERNEL_CORES > 1
#error "one kernel over several x86_64 cores has no per-core panic seat here"
#endif
    .balign 16
    .globl kickos_panic_stack_enter
kickos_panic_stack_enter:
    cli
    movq    %rcx, %rsp
    pushq   $0
    jmp     kickos_panic_report
