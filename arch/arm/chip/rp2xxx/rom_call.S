/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * uintptr_t kickos_rp_rom_call(struct kickos_rp_rom_args const* args), rp2xxx.h: args->fn(a0..a3)
 * with interrupts masked, on this core's slice of kickos_rp_rom_stack, returning fn's r0. A
 * bootrom routine's stack use is in no call graph. Thumb-1, for ARMv6-M.
 */
    .syntax unified
    .thumb

    .section .text.kickos_rp_rom_call, "ax", %progbits
    .global kickos_rp_rom_call
    .type kickos_rp_rom_call, %function
    .thumb_func
kickos_rp_rom_call:
    push    {r4, r5, r6, lr}
    mrs     r6, primask
    cpsid   i
    mov     r4, sp
    ldr     r5, =0xD0000000         /* SIO CPUID: 0 or 1 on both parts */
    ldr     r5, [r5]
    adds    r5, r5, #1
    lsls    r5, r5, #9              /* the top of this core's 512-byte slice */
    ldr     r1, =kickos_rp_rom_stack
    adds    r5, r5, r1
    mov     sp, r5
    ldr     r5, [r0, #0]
    ldr     r1, [r0, #8]
    ldr     r2, [r0, #12]
    ldr     r3, [r0, #16]
    ldr     r0, [r0, #4]
    blx     r5
    mov     sp, r4
    msr     primask, r6
    pop     {r4, r5, r6, pc}
    .size kickos_rp_rom_call, . - kickos_rp_rom_call
    .ltorg

    /* One slice per core of the part, so two cores' calls never share one. Touched by the call
     * above alone; 8-aligned, as AAPCS asks of sp at a call. */
    .section .bss.kickos_rp_rom_stack, "aw", %nobits
    .balign 8
kickos_rp_rom_stack:
    .space 1024
