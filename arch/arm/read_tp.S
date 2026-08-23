/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * __aeabi_read_tp, shared by armv6m and armv7m. Defined by NEITHER libc.a NOR libgcc.a on
 * the arm-none-eabi toolchain: the compiler emits a call to it for every thread_local
 * access it cannot cache, and the kernel is what has to answer.
 */

#if defined(KICKOS_TLS) && KICKOS_TLS

    .syntax unified
    .thumb
    .section .text.__aeabi_read_tp, "ax", %progbits
    .balign 2
    .global __aeabi_read_tp
    .thumb_func
    .type   __aeabi_read_tp, %function

/* Runs UNPRIVILEGED in the calling thread, so it cannot read the current TCB from a kernel
 * global: that load faults on an enforcing board. M-profile gives unprivileged code no
 * register that differs per thread except SP, which is what arch_ram_region_align strides
 * every arena block by KICKOS_TLS_STRIDE for.
 *
 * AEABI restricted clobber: r0, ip, lr and the flags ONLY, so no scratch register and no
 * memory access may be added here.
 *
 * Shifts and not BIC: a Thumb-2 BIC immediate is a modified immediate and 0x1FFF is not one.
 *
 * The subtract covers the edge of the range: a stack top is EXCLUSIVE, so an empty stack has
 * SP at base + stride, which a plain mask sends to the NEXT block. SP-1 puts every SP in
 * (base, base + stride] on base. SP reaching base would need the block itself overrun, and
 * stack_lo sits above it.
 */
__aeabi_read_tp:
    mov     r0, sp
    subs    r0, r0, #1
    lsrs    r0, r0, #KICKOS_TLS_STRIDE_LOG2
    lsls    r0, r0, #KICKOS_TLS_STRIDE_LOG2
    bx      lr
    .size   __aeabi_read_tp, . - __aeabi_read_tp

#endif
