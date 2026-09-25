/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The six window over/underflow vectors are mandatory: any call4/8/12 that outruns
 * the physical register file traps into them. Their s32e/l32e offsets and rfwo/rfwu
 * returns are the ISA-canonical handlers, and the VECBASE offsets are the fixed LX6
 * layout (XCHAL_*_VECOFS).
 */

/* PS bit fields (corebits.h): INTLEVEL[3:0], EXCM=bit4(0x10), UM=bit5(0x20),
 * WOE=bit18(0x40000). LX6 EXCM_LEVEL=3. */
    .set PS_SETUP,  0x0001F        /* INTLEVEL=15, EXCM=1, WOE=0 (window machinery off) */
    .set PS_BOOT,   0x4002F        /* WOE=1, UM=1, INTLEVEL=15, EXCM=0 (windowed C boot) */
    .set PS_ISR_C,  0x40021        /* WOE=1, UM=1, INTLEVEL=1, EXCM=0 (C ISR body) */
    .set PS_SPILL,  0x40023        /* WOE=1, UM=1, INTLEVEL=3, EXCM=0 (== switch.S) */
    .set PS_EXCM,   0x10

    .set EXCCAUSE_LEVEL1INTERRUPT, 4

/* arch_context field offsets and resume_kind values (context.h, guarded by the
   static_asserts in arch_xtensa.cc). Must match switch.S. */
    .set CTX_SP,    0
    .set CTX_PS,    4
    .set CTX_PC,    8
    .set CTX_KIND,  12
    .set CTX_TID,   16             /* trace_tid (telemetry builds only) */
    .set RESUME_IRQ, 1

/* SPILL_ALL_WINDOWS: flush every live ancestor window of the current call chain
   to its stack, leaving WindowStart with a single live frame. Requires WOE=1,
   EXCM=0, INTLEVEL>=EXCM_LEVEL (PS_SPILL). The 64-AR rotw sequence is the one the
   Xtensa ISA Reference Manual mandates. Must stay identical to the copy in
   arch/xtensa/lx6/switch.S. */
    .macro SPILL_ALL_WINDOWS
    and     a12, a12, a12
    rotw    3
    and     a12, a12, a12
    rotw    3
    and     a12, a12, a12
    rotw    3
    and     a12, a12, a12
    rotw    3
    and     a12, a12, a12
    rotw    4
    .endm

#include <kickos/arch/xtensa_frame.h>
#include <kickos/arch/lx6_switch_unlock.inc>
#include <kickos/arch/lx6_percpu.inc>

    .text

/* ------------------------------------------------------------------------- */
/* Reset entry                                                               */
/* ------------------------------------------------------------------------- */
/* CORE_PROLOGUE: every core must run this for itself before its first windowed call.
 * PS, WindowBase, WindowStart and VECBASE are per-core registers, and a core that skips
 * this takes its first window overflow into the ROM vectors with no report. */
    .macro  CORE_PROLOGUE stack_top
    /* Window machinery off so the window-state writes below cannot trap. */
    movi    a0, PS_SETUP
    wsr.ps  a0
    rsync
    /* Reset does not guarantee WindowBase/WindowStart: seat base 0, window 0 live. */
    movi    a0, 0
    wsr.windowbase a0
    rsync
    movi    a0, 1
    wsr.windowstart a0
    rsync
    /* Both cores point at one table: the vectors touch nothing but EXCSAVE1 and the
       faulting thread's own stack. */
    movi    a1, \stack_top
    movi    a0, _kickos_vectors
    wsr.vecbase a0
    rsync
    /* Interrupts stay masked until the first thread runs. */
    movi    a0, PS_BOOT
    wsr.ps  a0
    rsync
    .endm

    .align 4
    .global _start
    .type _start, @function
_start:
    CORE_PROLOGUE _estack
    /* Does not return. The call4 reach relies on -mlongcalls. */
    call4   Reset_Handler
1:  j       1b
    .size _start, .-_start

#if KICKOS_NUM_CORES > 1
/* ------------------------------------------------------------------------- */
/* APP_CPU reset entry                                                       */
/* ------------------------------------------------------------------------- */
    .align 4
    .global _kickos_lx6_core1_entry
    .type _kickos_lx6_core1_entry, @function
_kickos_lx6_core1_entry:
    CORE_PROLOGUE _estack_core1
    call4   kickos_lx6_secondary_entry
1:  j       1b
    .size _kickos_lx6_core1_entry, .-_kickos_lx6_core1_entry
#endif

/* ------------------------------------------------------------------------- */
/* Common halt                                                               */
/* ------------------------------------------------------------------------- */
    .align 4
    .global _kickos_lx6_halt
    .type _kickos_lx6_halt, @function
_kickos_lx6_halt:
    rsil    a0, 15
    /* waiti writes PS.INTLEVEL from its immediate: `waiti 0` would unmask what the
       rsil just masked. */
1:  waiti   15
    j       1b
    .size _kickos_lx6_halt, .-_kickos_lx6_halt

/* ------------------------------------------------------------------------- */
/* Unhandled synchronous exception                                           */
/* ------------------------------------------------------------------------- */
/* Entered EXCM=1 with a0 = EXCCAUSE and a1 = the interruptee SP. The special registers
   must reach the call4 argument registers (a6-a9) before WOE is re-enabled: a window
   overflow overwrites EPC1/EXCVADDR. Assumes a1 is usable, so a stack fault cannot be
   reported. */
    .align 4
    .global _kickos_lx6_fault
    .type _kickos_lx6_fault, @function
_kickos_lx6_fault:
    mov     a6, a0                /* arg0 = EXCCAUSE (from _UserExceptionVector) */
    rsr.ps  a9                    /* arg3 = PS at fault (EXCM=1); read before we change it */
    rsr.excvaddr a7               /* arg1 = EXCVADDR */
    rsr.epc1 a8                   /* arg2 = EPC1 (faulting PC) */
    movi    a0, PS_BOOT           /* WOE=1, UM=1, INTLEVEL=15, EXCM=0: C-callable posture */
    wsr.ps  a0
    rsync
    call4   kickos_lx6_fault_report
    j       _kickos_lx6_halt
    .size _kickos_lx6_fault, .-_kickos_lx6_fault

/* ------------------------------------------------------------------------- */
/* Level-1 interrupt entry                                                   */
/* ------------------------------------------------------------------------- */
/* Entered EXCM=1 with EPC1 = interrupted PC, original a0 in EXCSAVE1 and a1 =
   interruptee SP. */
    .align 4
    .global _kickos_int_level1
    .type _kickos_int_level1, @function
_kickos_int_level1:
    addi    a1, a1, -F_SIZE       /* frame below the interruptee SP */
    s32i    a2, a1, F_A2          /* free a2 as scratch */
    rsr.epc1 a2
    s32i    a2, a1, F_PC
    rsr.ps  a2
    s32i    a2, a1, F_PS          /* interruptee PS (EXCM=1); rfe clears EXCM */
    rsr.sar a2
    s32i    a2, a1, F_SAR
    rsr.lbeg a2
    s32i    a2, a1, F_LBEG
    rsr.lend a2
    s32i    a2, a1, F_LEND
    rsr.lcount a2
    s32i    a2, a1, F_LCNT
    rsr.excsave1 a2               /* original a0 */
    s32i    a2, a1, F_A0
    addi    a2, a1, F_SIZE
    s32i    a2, a1, F_A1          /* interruptee SP */
    s32i    a3,  a1, F_A3
    s32i    a4,  a1, F_A4
    s32i    a5,  a1, F_A5
    s32i    a6,  a1, F_A6
    s32i    a7,  a1, F_A7
    s32i    a8,  a1, F_A8
    s32i    a9,  a1, F_A9
    s32i    a10, a1, F_A10
    s32i    a11, a1, F_A11
    s32i    a12, a1, F_A12
    s32i    a13, a1, F_A13
    s32i    a14, a1, F_A14
    s32i    a15, a1, F_A15

    /* FPU (CP0): f-regs are caller-saved, so this preemptive path is the only switch
       path that banks them. The ops are gated by CPENABLE (CP0 enabled globally in
       kickos_lx6_init), not by EXCM. */
    ssi     f0,  a1, F_F0
    ssi     f1,  a1, F_F0 + 4
    ssi     f2,  a1, F_F0 + 8
    ssi     f3,  a1, F_F0 + 12
    ssi     f4,  a1, F_F0 + 16
    ssi     f5,  a1, F_F0 + 20
    ssi     f6,  a1, F_F0 + 24
    ssi     f7,  a1, F_F0 + 28
    ssi     f8,  a1, F_F0 + 32
    ssi     f9,  a1, F_F0 + 36
    ssi     f10, a1, F_F0 + 40
    ssi     f11, a1, F_F0 + 44
    ssi     f12, a1, F_F0 + 48
    ssi     f13, a1, F_F0 + 52
    ssi     f14, a1, F_F0 + 56
    ssi     f15, a1, F_F0 + 60
    rur.fcr a2
    s32i    a2, a1, F_FCR
    rur.fsr a2
    s32i    a2, a1, F_FSR

    /* Flush the interruptee's ancestor windows to its own stack: a frame that never
       overflowed lives only in the register file, where the next thread would clobber
       it. Both resume paths (retw and rfe, see switch.S) rely on the single live frame
       this leaves. a1 must be the interruptee's original sp (F_A1) so each ancestor
       spills to its real base save area; a1 is the only register the spill preserves.
       The spill clobbers EPC1; F_PC already holds it. */
    l32i    a1, a1, F_A1          /* a1 = interruptee original sp */
    movi    a2, PS_SPILL
    wsr.ps  a2
    rsync
    SPILL_ALL_WINDOWS
    movi    a2, F_SIZE
    sub     a1, a1, a2            /* a1 = frame base (orig sp - F_SIZE) */

    /* arch_in_isr() depth, this core's cell. */
    PERCPU_CELL a2, g_isr_depth, a3
    l32i    a3, a2, 0
    addi    a3, a3, 1
    s32i    a3, a2, 0

    /* INTLEVEL=1 masks level-1 re-entry and leaves higher levels open. */
    movi    a2, PS_ISR_C
    wsr.ps  a2
    rsync
    call4   kickos_lx6_dispatch_l1

    /* --- interrupt exit + deferred-switch completion ------------------------
       PS is still PS_ISR_C and a1 the frame base. WindowStart is a single live
       frame, which both resume tails below require. */
    PERCPU_CELL a2, g_isr_depth, a3
    l32i    a3, a2, 0
    addi    a3, a3, -1
    s32i    a3, a2, 0

    PERCPU_CELL a2, g_arch_switch_pending, a4
    l32i    a3, a2, 0
    beqz    a3, _kickos_lx6_irq_restore
    movi    a3, 0
    s32i    a3, a2, 0             /* consume the request */

    /* The preempted thread's frame at a1 is already complete; bank it as IRQ. */
    PERCPU_CELL a2, g_arch_current, a3
    l32i    a2, a2, 0             /* a2 = interruptee ctx* (seeded by arch_start) */
    s32i    a1, a2, CTX_SP        /* current->sp = frame base */
    movi    a3, RESUME_IRQ
    s32i    a3, a2, CTX_KIND
#if KICKOS_TELEMETRY
    /* call4 is legal here (EXCM=0, WOE=1, single-frame window); a1/a2 survive it. */
    PERCPU_CELL a3, g_arch_next, a4
    l32i    a3, a3, 0
    l32i    a6, a2, CTX_TID       /* from_tid */
    l32i    a7, a3, CTX_TID       /* to_tid */
    call4   kickos_trace_switch_done
#endif
    PERCPU_CELL a2, g_arch_next, a4
    l32i    a3, a2, 0             /* a3 = incoming ctx* */
    PERCPU_CELL a2, g_arch_current, a4
    s32i    a3, a2, 0

    /* An interrupt-decided switch never enters xtensa_switch: without this seat the
       incoming thread runs on the outgoing thread's THREADPTR until its next
       cooperative switch. */
    SEAT_THREADPTR a3, a2

    l32i    a2, a3, CTX_KIND
    bnez    a2, .Lint_resume_irq
    /* COOP: with a single live frame WindowBase does not matter; the underflow
       rebuilds the incoming chain from incoming->sp. */
    l32i    a1, a3, CTX_SP        /* incoming->sp (switch frame) */
    SWITCH_UNLOCK                 /* standing on the incoming frame; a3 survives call4 */
    l32i    a4, a3, CTX_PS
    l32i    a0, a3, CTX_PC        /* return addr, carries CALLINC */
    wsr.ps  a4
    rsync
    retw
.Lint_resume_irq:
    l32i    a1, a3, CTX_SP        /* incoming frame base -> shared rfe epilogue */
    SWITCH_UNLOCK
    .size _kickos_int_level1, .-_kickos_int_level1

/* ------------------------------------------------------------------------- */
/* Shared rfe restore epilogue                                               */
/* ------------------------------------------------------------------------- */
/* Entry: a1 = interrupt frame base, WindowStart a single live frame. Also entered from
   thread context (switch.S), so it raises EXCM itself: the restore and rfe must not be
   interrupted mid-sequence. */
    .global _kickos_lx6_irq_restore
    .type _kickos_lx6_irq_restore, @function
_kickos_lx6_irq_restore:
    rsr.ps  a2
    movi    a3, PS_EXCM
    or      a2, a2, a3
    wsr.ps  a2
    rsync

    l32i    a2, a1, F_LCNT
    wsr.lcount a2
    l32i    a2, a1, F_LEND
    wsr.lend a2
    l32i    a2, a1, F_LBEG
    wsr.lbeg a2
    l32i    a2, a1, F_SAR
    wsr.sar a2
    l32i    a2, a1, F_PC
    wsr.epc1 a2
    l32i    a2, a1, F_PS
    wsr.ps  a2                    /* restore interruptee PS; rfe clears its EXCM */

    /* FPU (CP0): mirrors the level-1 entry save. */
    lsi     f0,  a1, F_F0
    lsi     f1,  a1, F_F0 + 4
    lsi     f2,  a1, F_F0 + 8
    lsi     f3,  a1, F_F0 + 12
    lsi     f4,  a1, F_F0 + 16
    lsi     f5,  a1, F_F0 + 20
    lsi     f6,  a1, F_F0 + 24
    lsi     f7,  a1, F_F0 + 28
    lsi     f8,  a1, F_F0 + 32
    lsi     f9,  a1, F_F0 + 36
    lsi     f10, a1, F_F0 + 40
    lsi     f11, a1, F_F0 + 44
    lsi     f12, a1, F_F0 + 48
    lsi     f13, a1, F_F0 + 52
    lsi     f14, a1, F_F0 + 56
    lsi     f15, a1, F_F0 + 60
    l32i    a2, a1, F_FCR
    wur.fcr a2
    l32i    a2, a1, F_FSR
    wur.fsr a2

    l32i    a3,  a1, F_A3
    l32i    a4,  a1, F_A4
    l32i    a5,  a1, F_A5
    l32i    a6,  a1, F_A6
    l32i    a7,  a1, F_A7
    l32i    a8,  a1, F_A8
    l32i    a9,  a1, F_A9
    l32i    a10, a1, F_A10
    l32i    a11, a1, F_A11
    l32i    a12, a1, F_A12
    l32i    a13, a1, F_A13
    l32i    a14, a1, F_A14
    l32i    a15, a1, F_A15
    l32i    a0,  a1, F_A0
    l32i    a2,  a1, F_A2
    l32i    a1,  a1, F_A1         /* pop the frame (restore interruptee SP) */
    rsync
    rfe
    .size _kickos_lx6_irq_restore, .-_kickos_lx6_irq_restore

/* ------------------------------------------------------------------------- */
/* VECBASE vector table                                                      */
/* ------------------------------------------------------------------------- */
    .section .vectors, "ax"

/* --- WindowOverflow4 @ 0x000: spill a0-a3 to the callee frame base save area. --- */
    .org 0x000
_WindowOverflow4:
    s32e    a0, a5, -16
    s32e    a1, a5, -12
    s32e    a2, a5, -8
    s32e    a3, a5, -4
    rfwo

/* --- WindowUnderflow4 @ 0x040: reload a0-a3 from the base save area. ---------- */
    .org 0x040
_WindowUnderflow4:
    l32e    a0, a5, -16
    l32e    a1, a5, -12
    l32e    a2, a5, -8
    l32e    a3, a5, -4
    rfwu

/* --- WindowOverflow8 @ 0x080: a0-a3 to callee frame, a4-a7 to caller frame. --- */
    .org 0x080
_WindowOverflow8:
    s32e    a0, a9, -16
    l32e    a0, a1, -12           /* a0 <- caller's caller SP (to reach a4-a7 area) */
    s32e    a1, a9, -12
    s32e    a2, a9, -8
    s32e    a3, a9, -4
    s32e    a4, a0, -32
    s32e    a5, a0, -28
    s32e    a6, a0, -24
    s32e    a7, a0, -20
    rfwo

/* --- WindowUnderflow8 @ 0x0C0. ------------------------------------------------ */
    .org 0x0C0
_WindowUnderflow8:
    l32e    a0, a9, -16
    l32e    a1, a9, -12
    l32e    a2, a9, -8
    l32e    a7, a1, -12
    l32e    a3, a9, -4
    l32e    a4, a7, -32
    l32e    a5, a7, -28
    l32e    a6, a7, -24
    l32e    a7, a7, -20
    rfwu

/* --- WindowOverflow12 @ 0x100: a0-a3 to callee frame, a4-a11 to caller frame. - */
    .org 0x100
_WindowOverflow12:
    s32e    a0, a13, -16
    l32e    a0, a1, -12
    s32e    a1, a13, -12
    s32e    a2, a13, -8
    s32e    a3, a13, -4
    s32e    a4, a0, -48
    s32e    a5, a0, -44
    s32e    a6, a0, -40
    s32e    a7, a0, -36
    s32e    a8, a0, -32
    s32e    a9, a0, -28
    s32e    a10, a0, -24
    s32e    a11, a0, -20
    rfwo

/* --- WindowUnderflow12 @ 0x140. ----------------------------------------------- */
    .org 0x140
_WindowUnderflow12:
    l32e    a0, a13, -16
    l32e    a1, a13, -12
    l32e    a2, a13, -8
    l32e    a11, a1, -12
    l32e    a3, a13, -4
    l32e    a4, a11, -48
    l32e    a5, a11, -44
    l32e    a6, a11, -40
    l32e    a7, a11, -36
    l32e    a8, a11, -32
    l32e    a9, a11, -28
    l32e    a10, a11, -24
    l32e    a11, a11, -20
    rfwu

/* --- Level 2-5 / Debug(6) / NMI(7): not handled -> halt. ---------------------- */
    .org 0x180
_Level2InterruptVector: j _kickos_lx6_halt
    .org 0x1C0
_Level3InterruptVector: j _kickos_lx6_halt
    .org 0x200
_Level4InterruptVector: j _kickos_lx6_halt
    .org 0x240
_Level5InterruptVector: j _kickos_lx6_halt
    .org 0x280
_DebugExceptionVector:  j _kickos_lx6_halt
    .org 0x2C0
_NMIExceptionVector:    j _kickos_lx6_halt

/* --- Kernel exception @ 0x300: an exception taken while EXCM=1 -> halt. -------- */
    .org 0x300
_KernelExceptionVector: j _kickos_lx6_halt

/* --- User exception @ 0x340: level-1 interrupts (EXCCAUSE=4) go to the ISR       */
/*     entry; any other synchronous cause halts. ------------------------------- */
    .org 0x340
_UserExceptionVector:
    wsr.excsave1 a0               /* free a0 (original a0 preserved in EXCSAVE1) */
    rsr.exccause a0
    beqi    a0, EXCCAUSE_LEVEL1INTERRUPT, 1f
    j       _kickos_lx6_fault     /* unhandled synchronous exception -> tag + halt */
1:  j       _kickos_int_level1

/* --- Double exception @ 0x3C0: fatal -> halt. --------------------------------- */
    .org 0x3C0
_DoubleExceptionVector: j _kickos_lx6_halt

    .org 0x400
