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

/* IT RUNS UNPRIVILEGED, IN THE CALLING THREAD, WITH THAT THREAD'S GRANTS AND NOTHING MORE.
 * So it cannot read the current TCB out of a kernel global the way an RTOS with no MPU
 * would: on an enforcing board that load faults. M-profile hands unprivileged code no
 * register that differs per thread except SP, which is why the answer is derived from SP
 * and why every arena block is strided by KICKOS_TLS_STRIDE (arch_ram_region_align).
 *
 * Shifting rather than BIC because the mask is not encodable: a Thumb-2 BIC immediate is a
 * modified immediate and 0x1FFF is not one.
 *
 * AEABI restricted clobber: this may modify r0, ip, lr and the flags ONLY, so r1-r3 and
 * every VFP register survive it. Three instructions and no memory access satisfy that with
 * room to spare, which is also why it cannot be written in C.
 *
 * THE SUBTRACT IS NOT DEFENSIVE, IT IS THE EDGE OF THE RANGE. A stack top is EXCLUSIVE, so
 * an empty stack has SP exactly at base + stride, which masks to the NEXT block and hands
 * the thread its neighbour's storage. It is reachable: the first run of the tlsprobe
 * witness took a data-abort at the neighbour's block for precisely this, and adding one
 * unrelated local to main moved SP and hid it again. Masking SP-1 puts every SP in
 * (base, base + stride] on base. SP == base cannot occur: the TLS block itself occupies the
 * bottom of the stack and stack_lo is raised above it.
 */
__aeabi_read_tp:
    mov     r0, sp
    subs    r0, r0, #1
    lsrs    r0, r0, #KICKOS_TLS_STRIDE_LOG2
    lsls    r0, r0, #KICKOS_TLS_STRIDE_LOG2
    bx      lr
    .size   __aeabi_read_tp, . - __aeabi_read_tp

#endif
