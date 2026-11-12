/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The user side of the syscall trap: arch_syscall(nr, a0, a1, a2, a3), arch_syscall64 the same
 * entry, and arch_syscall_reg where the arch carries the register-form IPC trap. Part of the user
 * runtime, so every image carries its own.
 */

#include <kickos/arch/rx_trap_stack.h>

    .section .text.kickos_arch_syscall, "ax", @progbits

/* arch_syscall(nr, a0, a1, a2, a3): R1=nr R2=a0 R3=a1 R4=a2, [R0+4]=a3. INT     */
/* preserves all GPRs, so no marshaling is needed. arch_syscall64 is the SAME    */
/* trap: R1:R2 is the RX ABI's 8-byte return pair.                               */
    .global _arch_syscall
    .global _arch_syscall64
_arch_syscall:
_arch_syscall64:
    int     #1                 /* SYSCALL_VECTOR (regs.h) */
    rts                        /* return to caller, R1:R2 = result */

/* int32_t arch_syscall_reg(uint32_t* io): R1 = io. INT preserves every GPR, so  */
/* the payload rides R4-R8 straight into the trap and the trap's own frame build */
/* is what stores it. R6-R8 are callee-saved and are banked here. RTE lands after */
/* the INT whether the trap took the fastpath or ran the generic dispatch.        */
/* The fastpath banks the DPFPU file at a fixed distance below the USP the INT    */
/* leaves, so R0 is aligned for it here whatever the caller's frames add up to.   */
    .global _arch_syscall_reg
_arch_syscall_reg:
    pushm   r6-r8
    mov.l   r0, r14
    sub     #8, r0
    and     #-KICKOS_RX_TRAP_BANK_ALIGN, r0
    mov.l   r14, [r0]          /* the SP to return on */
    mov.l   r1, 4[r0]          /* io, across the trap */
    mov.l   4[r1], r2          /* io[1] = endpoint cap */
    mov.l   8[r1], r3          /* io[2] = packed lengths */
    mov.l   12[r1], r4         /* io[3..7] = the request payload */
    mov.l   16[r1], r5
    mov.l   20[r1], r6
    mov.l   24[r1], r7
    mov.l   28[r1], r8
    mov.l   [r1], r1           /* nr, last: it overwrites the pointer */
    int     #1                 /* SYSCALL_VECTOR (regs.h) */
    /* R2-R6 carry the reply on the taken path and the untouched request on the declined one.
       The stub reads them only when the result is not KOS_CALL_REG_FALLBACK, so the
       unconditional store is observationally free on the declined path. */
    mov.l   4[r0], r14         /* io */
    mov.l   r2, 4[r14]
    mov.l   r3, 8[r14]
    mov.l   r4, 12[r14]
    mov.l   r5, 16[r14]
    mov.l   r6, 20[r14]
    mov.l   [r0], r0
    popm    r6-r8
    rts
