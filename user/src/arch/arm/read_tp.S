/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * __aeabi_read_tp, shared by armv6m and armv7m; neither libc.a nor libgcc.a defines it.
 */

#if KICKOS_TLS

    .syntax unified
    .thumb
    .section .text.__aeabi_read_tp, "ax", %progbits
    .balign 2
    .global __aeabi_read_tp
    .thumb_func
    .type   __aeabi_read_tp, %function

/* Runs UNPRIVILEGED in the calling thread, so it cannot read the current TCB from a kernel
 * global: that load faults on an enforcing board. M-profile gives unprivileged code no
 * register that differs per thread except SP, which is why arch_ram_region_align aligns every
 * arena block of exactly KICKOS_TLS_STRIDE, the only size a stack can be, to the stride.
 *
 * AEABI restricted clobber: r0, ip, lr and the flags ONLY, so no scratch register may be added
 * here, and the one load is of this image's own literal, in its code.
 *
 * Shifts and not BIC: a Thumb-2 BIC immediate is a modified immediate and 0x1FFF is not one.
 *
 * The subtract covers the edge of the range: a stack top is EXCLUSIVE, so an empty stack has
 * SP at base + stride, which a plain mask sends to the NEXT block. SP-1 puts every SP in
 * (base, base + stride] on base. SP reaching base would need the block itself overrun, and
 * stack_lo sits above it.
 *
 * The TLS block starts KICKOS_REENT_CARVE above the base, past the thread's libc state. Two
 * halves: an armv6m ADDS immediate is eight bits.
 *
 * The thread pointer is then the block less this image's offset in the link's one thread-local
 * segment (<kickos/sys/image.h>), so the image's own offsets, which the link measures from the
 * segment's start, land in its block. The kernel seats the block from the same offset.
 */
#if KICKOS_REENT_CARVE % 2 != 0 || KICKOS_REENT_CARVE / 2 > 255
#error "__aeabi_read_tp adds KICKOS_REENT_CARVE as two eight-bit immediates"
#endif
__aeabi_read_tp:
    mov     r0, sp
    subs    r0, r0, #1
    lsrs    r0, r0, #KICKOS_TLS_STRIDE_LOG2
    lsls    r0, r0, #KICKOS_TLS_STRIDE_LOG2
    adds    r0, r0, #(KICKOS_REENT_CARVE / 2)
    adds    r0, r0, #(KICKOS_REENT_CARVE / 2)
    mov     ip, r0
    ldr     r0, .Ltls_offset
    negs    r0, r0
    add     r0, r0, ip
    bx      lr
    .balign 4
.Ltls_offset:
    .word   __kickos_image_tls_offset
    .size   __aeabi_read_tp, . - __aeabi_read_tp

#endif
