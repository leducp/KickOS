/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * ESP32 (Xtensa LX6) startup: the reset entry (_start), the VECBASE exception/
 * interrupt vector table, and the first-level (level-1) interrupt entry. The ROM
 * loader copies the image segments into internal SRAM and jumps to _start (the
 * ELF entry). _start initializes the register-window state, sets the stack, points
 * VECBASE at this table, and makes the first WINDOWED call into C.
 *
 * WINDOWED ABI (Xtensa ISA, Windowed Register Option): the six window over/
 * underflow vectors are MANDATORY -- any call4/8/12 that outruns the physical
 * register file traps here to spill/fill a frame's registers to/from its stack
 * base save area. The exact s32e/l32e offsets and rfwo/rfwu returns are the
 * ISA-canonical handlers. Vector VECBASE offsets are the fixed LX6 layout
 * (XCHAL_*_VECOFS): window vectors at 0x000..0x140 (64 bytes apart), the level
 * interrupt / exception vectors at 0x180..0x3C0.
 *
 * Level-1 interrupts (the CCOMPARE0 timer is internal line 6, a level-1 line)
 * arrive at the User exception vector with EXCCAUSE=LEVEL1INTERRUPT(4); the entry
 * saves the interruptee's state, dispatches to C, and returns via rfe. The
 * PREEMPTIVE switch-on-ISR-exit is a documented TODO(HW) (see _kickos_int_level1).
 */

/* PS bit fields (corebits.h): INTLEVEL[3:0], EXCM=bit4(0x10), UM=bit5(0x20),
 * WOE=bit18(0x40000). LX6 EXCM_LEVEL=3. */
    .set PS_SETUP,  0x0001F        /* INTLEVEL=15, EXCM=1, WOE=0 (window machinery off) */
    .set PS_BOOT,   0x4002F        /* WOE=1, UM=1, INTLEVEL=15, EXCM=0 (windowed C boot) */
    .set PS_ISR_C,  0x40021        /* WOE=1, UM=1, INTLEVEL=1, EXCM=0 (C ISR body) */
    .set PS_SPILL,  0x40023        /* WOE=1, UM=1, INTLEVEL=3, EXCM=0 (== switch.S) */
    .set PS_EXCM,   0x10

    .set EXCCAUSE_LEVEL1INTERRUPT, 4

/* arch_context field offsets + resume_kind values (context.h; guarded by the
   static_asserts in arch_xtensa.cc). Must match switch.S. */
    .set CTX_SP,    0
    .set CTX_PS,    4
    .set CTX_PC,    8
    .set CTX_KIND,  12
    .set CTX_TID,   16             /* trace_tid (telemetry builds only) */
    .set RESUME_IRQ, 1

/* SPILL_ALL_WINDOWS: flush every live ancestor window of the current call chain
   to its stack, leaving WindowStart with a single live frame. Requires WOE=1,
   EXCM=0, INTLEVEL>=EXCM_LEVEL (PS_SPILL). The 64-AR rotw sequence mandated by
   the Xtensa ISA Reference Manual (Windowed Register Option) -- an ISA-dictated
   mechanism. Kept identical to the copy in arch/xtensa/lx6/switch.S -- both files
   need it and they are assembled separately. */
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

/* Level-1 interrupt frame (built on the interruptee's stack). 256 bytes. */
    .set F_PC,   0x00
    .set F_PS,   0x04
    .set F_SAR,  0x08
    .set F_LBEG, 0x0C
    .set F_LEND, 0x10
    .set F_LCNT, 0x14
    .set F_A0,   0x20              /* a0..a15 contiguous, 4 bytes each */
    .set F_A1,   0x24
    .set F_A2,   0x28
    .set F_A3,   0x2C
    .set F_A4,   0x30
    .set F_A5,   0x34
    .set F_A6,   0x38
    .set F_A7,   0x3C
    .set F_A8,   0x40
    .set F_A9,   0x44
    .set F_A10,  0x48
    .set F_A11,  0x4C
    .set F_A12,  0x50
    .set F_A13,  0x54
    .set F_A14,  0x58
    .set F_A15,  0x5C
    /* Single-precision FPU (CP0) save area: f0-f15 then FCR, FSR. Fits the frame's
       existing slack (the a-regs end at 0x5C; 0x60..0xA7 was unused). FP is banked
       ONLY on the preemptive path -- the compiler spills live f-regs across the
       cooperative switch's call (caller-saved), so xtensa_switch needs none. */
    .set F_F0,   0x60              /* f0..f15 contiguous, 4 bytes each -> ..0x9C */
    .set F_FCR,  0xA0              /* FP control (0x60 + 16*4) */
    .set F_FSR,  0xA4              /* FP status */
    .set F_SIZE, 0x100

    .text

/* ------------------------------------------------------------------------- */
/* Reset entry: the image's ELF entry point (ENTRY(_start)).                    */
/* ------------------------------------------------------------------------- */
    .align 4
    .global _start
    .type _start, @function
_start:
    /* Setup mode: window machinery off so the window-state writes below cannot
       trap. */
    movi    a0, PS_SETUP
    wsr.ps  a0
    rsync
    /* Window state: base 0, only window 0 live (a bare-metal reset must not trust
       leftover WindowBase/WindowStart). */
    movi    a0, 0
    wsr.windowbase a0
    rsync
    movi    a0, 1
    wsr.windowstart a0
    rsync
    /* Stack + relocated vectors. */
    movi    a1, _estack
    movi    a0, _kickos_vectors
    wsr.vecbase a0
    rsync
    /* Enable the windowed ABI (WOE=1), keep interrupts masked until the first
       thread runs. */
    movi    a0, PS_BOOT
    wsr.ps  a0
    rsync
    /* First WINDOWED call into C (call4 rotates the window; Reset_Handler's entry
       allocates its frame). Does not return. -mlongcalls relaxes the range. */
    call4   Reset_Handler
1:  j       1b
    .size _start, .-_start

/* ------------------------------------------------------------------------- */
/* Common halt: mask everything (incl. NMI) and park. noreturn.                */
/* ------------------------------------------------------------------------- */
    .align 4
    .global _kickos_lx6_halt
    .type _kickos_lx6_halt, @function
_kickos_lx6_halt:
    rsil    a0, 15
    /* waiti 15, NOT waiti 0: waiti writes PS.INTLEVEL from its immediate, so
       `waiti 0` would UNMASK everything the rsil just masked. Park masked. */
1:  waiti   15
    j       1b
    .size _kickos_lx6_halt, .-_kickos_lx6_halt

/* ------------------------------------------------------------------------- */
/* Unhandled synchronous exception (e.g. Coprocessor0Disabled from an FP insn   */
/* in the integer-only build -- see chip_esp32.cc). Capture the fault special      */
/* registers, then make a WINDOWED C call into the reporter to dump them over the  */
/* console before the shared dead-end. Entered EXCM=1 with a0 = EXCCAUSE (read by   */
/* _UserExceptionVector) and a1 = the interruptee SP. The four special regs go into */
/* the call4 argument registers (a6-a9) BEFORE the window ABI is re-enabled: a later */
/* window spill overwrites EPC1/EXCVADDR, but the captured GPR copies survive. Best- */
/* effort: assumes a1 is usable (a stack fault can't be dumped). noreturn.          */
/* ------------------------------------------------------------------------- */
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
/* Level-1 interrupt entry. Reached from _UserExceptionVector when EXCCAUSE ==   */
/* LEVEL1INTERRUPT. On entry: EXCM=1, EPC1 = interrupted PC, original a0 stashed   */
/* in EXCSAVE1, a1 = interruptee SP. Saves a full frame on the interruptee stack,  */
/* spills the interruptee's ancestor windows to its stack, runs the C dispatcher   */
/* at INTLEVEL=1, then on exit either resumes the interruptee (rfe) or, if a switch */
/* was requested from ISR context (g_arch_switch_pending), completes the deferred   */
/* swap: the preempted thread is banked in this interrupt-frame format and the      */
/* incoming thread is resumed by the path its resume_kind selects (retw for a       */
/* cooperatively-blocked thread, rfe for a previously-preempted one). This is the   */
/* PendSV/SWINT analog -- the switch decision is made in C during dispatch and the  */
/* physical swap happens here, on the way back to thread level.                     */
/* ------------------------------------------------------------------------- */
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

    /* FPU (CP0): a preempted thread may hold live f-regs at an arbitrary PC (FP is
       caller-saved, so this is the ONLY switch path that must bank them). CP0 is
       enabled globally (kickos_lx6_init); these coprocessor ops are gated by
       CPENABLE, not EXCM, so they run here. a1 = frame base. */
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

    /* Flush the interruptee's remaining live windows (its ancestor frames) to its
       OWN stack, so a switch-away at interrupt exit can abandon the register file
       without losing them. Mandatory: an ancestor frame that never overflowed
       lives only in the physical window file and would be clobbered by the next
       thread. It also leaves WindowStart a single live frame -- the precondition
       BOTH resume paths rely on (retw and rfe; see switch.S). The spill must run
       with a1 = the interruptee's ORIGINAL sp (F_A1), so each ancestor flushes to
       its real base save area and reloads from there on resume; a1 survives the
       spill (the only register that does), so recompute the frame base after.
       Window overflow clobbers EPC1, but F_PC already holds the saved PC. */
    l32i    a1, a1, F_A1          /* a1 = interruptee original sp */
    movi    a2, PS_SPILL
    wsr.ps  a2
    rsync
    SPILL_ALL_WINDOWS
    movi    a2, F_SIZE
    sub     a1, a1, a2            /* a1 = frame base (orig sp - F_SIZE) */

    /* arch_in_isr() depth. */
    movi    a2, g_isr_depth
    l32i    a3, a2, 0
    addi    a3, a3, 1
    s32i    a3, a2, 0

    /* C-callable posture: WOE=1, EXCM=0, INTLEVEL=1 (mask level-1 re-entry, allow
       higher). A window over/underflow during the C body spills/reloads the
       interruptee's ancestor frames onto its own stack -- correct and transparent. */
    movi    a2, PS_ISR_C
    wsr.ps  a2
    rsync
    call4   kickos_lx6_dispatch_l1

    /* --- interrupt exit + deferred-switch completion ------------------------
       Still WOE=1, EXCM=0, INTLEVEL=1 (PS_ISR_C, unchanged by the dispatcher's
       balanced call/retw). a1 = frame base. WindowStart is a single live frame
       (the entry spill + the dispatcher's balanced window use), so both resume
       tails below are legal here. */
    movi    a2, g_isr_depth
    l32i    a3, a2, 0
    addi    a3, a3, -1
    s32i    a3, a2, 0

    /* Preemption pending? (arch_switch set it from ISR context.) If not, resume the
       interruptee from its own frame -- the shared rfe epilogue with a1 = frame. */
    movi    a2, g_arch_switch_pending
    l32i    a3, a2, 0
    beqz    a3, _kickos_lx6_irq_restore
    movi    a3, 0
    s32i    a3, a2, 0             /* consume the request */

    /* Save the preempted thread (= g_arch_current) in the interrupt-frame format:
       its full frame is at a1, F_PC/F_PS/... already populated. Record the frame
       base and mark it IRQ so its next resume takes the rfe path. */
    movi    a2, g_arch_current
    l32i    a2, a2, 0             /* a2 = interruptee ctx* (seeded by arch_start) */
    s32i    a1, a2, CTX_SP        /* current->sp = frame base */
    movi    a3, RESUME_IRQ
    s32i    a3, a2, CTX_KIND
#if KICKOS_TELEMETRY
    /* SWITCH record: a2 = from ctx*, g_arch_next = to ctx*. call4 is legal here
       (EXCM=0, WOE=1, single-frame window); a1/a2 survive it (below its window). */
    movi    a3, g_arch_next
    l32i    a3, a3, 0
    l32i    a6, a2, CTX_TID       /* from_tid */
    l32i    a7, a3, CTX_TID       /* to_tid */
    call4   kickos_trace_switch_done
#endif
    /* g_arch_current = g_arch_next; load the incoming ctx*. */
    movi    a2, g_arch_next
    l32i    a3, a2, 0             /* a3 = incoming ctx* */
    movi    a2, g_arch_current
    s32i    a3, a2, 0

    /* Resume the incoming thread by the path its resume_kind selects. */
    l32i    a2, a3, CTX_KIND
    bnez    a2, .Lint_resume_irq
    /* COOP: retw into its cooperative continuation. Identical to xtensa_switch's
       retw resume -- the window file is a single frame, so WindowBase is irrelevant
       and the underflow rebuilds the incoming chain from incoming->sp. */
    l32i    a1, a3, CTX_SP        /* incoming->sp (switch frame) */
    l32i    a4, a3, CTX_PS
    l32i    a0, a3, CTX_PC        /* return addr, carries CALLINC */
    wsr.ps  a4
    rsync
    retw
.Lint_resume_irq:
    l32i    a1, a3, CTX_SP        /* incoming frame base -> shared rfe epilogue */
    .size _kickos_int_level1, .-_kickos_int_level1

/* ------------------------------------------------------------------------- */
/* Shared rfe restore epilogue. Entry: a1 = the base of a 256-byte interrupt    */
/* frame; WindowStart a single live frame. Reached three ways: no-switch exit,   */
/* a preemptive switch to a previously-preempted (IRQ) thread, and xtensa_switch  */
/* resuming such a thread from THREAD context (switch.S). Re-enters EXCM=1 so the */
/* PS/window restore + rfe are not interruptible mid-sequence; rfe clears EXCM.   */
/* ------------------------------------------------------------------------- */
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

    /* FPU (CP0): reload the banked f-regs + control/status before the a-regs (a2 is
       free scratch here; a1 is still the frame base). Symmetric with the L1-entry
       save; only threads resumed via this rfe path were preempted with live FP. */
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
/* VECBASE vector table. Window vectors are real (mandatory); the level/exception */
/* vectors route to the level-1 entry (interrupts) or halt (unhandled).           */
/* ------------------------------------------------------------------------- */
    .section .vectors, "ax"

/* --- WindowOverflow4 @ 0x000: spill a0-a3 to the callee frame base save area. -- */
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

/* --- Level 2-5 / Debug(6) / NMI(7): not handled build-only -> halt. ----------- */
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
