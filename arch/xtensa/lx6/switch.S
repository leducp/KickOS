/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Xtensa LX6 context switch (WINDOWED ABI). Unlike the ARM callee-save PendSV
 * swap, the windowed ABI keeps most of a thread's live register state in the
 * physical AR window file (64 ARs on LX6), reachable only through the sliding
 * a0-a15 window. So the cooperative switch first FLUSHES every live window of the
 * outgoing thread to that thread's OWN stack (SPILL_ALL_WINDOWS, a controlled
 * walk around the whole register file that forces each ancestor frame to overflow
 * to its base save area), then swaps the stack pointer, then resumes the incoming
 * thread with a windowed return (retw) -- whose underflow handler lazily reloads
 * the incoming caller's window from ITS stack.
 *
 * Why only 3 saved words (sp/ps/pc) suffice: after SPILL_ALL_WINDOWS the register
 * file holds only the current (switch) window; WindowStart has a single bit set.
 * The base-save-area convention (Xtensa ISA, Windowed Register Option) puts each
 * frame's a0-a3 -- including its a1 (SP) -- at [callee_sp - 16 .. callee_sp - 4].
 * A `retw` therefore rebuilds the entire incoming call chain from the incoming
 * thread's stack via the window-underflow vectors (startup.S). The physical
 * WindowBase value is irrelevant once the file is spilled, so resuming a thread
 * that suspended under a DIFFERENT WindowBase is safe. See the window vectors in
 * arch/xtensa/chip/esp32/startup.S.
 *
 * arch_context layout (context.h, offsets hard-coded here): sp@0, ps@4, pc@8.
 */

/* PS bit fields (Xtensa ISA / corebits.h): WOE=bit18, UM=bit5, EXCM=bit4,
 * INTLEVEL=[3:0]. EXCM_LEVEL on LX6 is 3. The spill needs WOE=1, EXCM=0, and
 * INTLEVEL >= EXCM_LEVEL so a window exception (not maskable) runs cleanly while
 * higher device lines stay masked. */
    .set PS_WOE,       0x40000
    .set PS_UM,        0x20
    .set PS_SPILL,     0x40023        /* WOE | UM | INTLEVEL=3 (EXCM=0) */

/* arch_context field offsets (context.h; the static_asserts in arch_xtensa.cc
   guard these). CTX_TID is telemetry-only. */
    .set CTX_SP,       0
    .set CTX_PS,       4
    .set CTX_PC,       8
    .set CTX_KIND,     12             /* resume_kind: 0=COOP (retw), 1=IRQ (rfe) */
    .set CTX_TID,      16             /* trace_tid (telemetry builds only) */
    .set RESUME_COOP,  0

/* SPILL_ALL_WINDOWS: XCHAL_NUM_AREGS == 64 on LX6, so rotate the window a full
 * 360 degrees (total rotw = 16 == 64/4). Each `and a12,a12,a12` is a no-op write
 * that, when a12 belongs to a still-live ancestor window, triggers a window
 * overflow that spills that frame to its stack; the walk guarantees every frame
 * but the current one is flushed. Requires PS as above. The 64-AR rotw-based
 * spill is the sequence mandated by the Xtensa ISA Reference Manual (Windowed
 * Register Option) -- an ISA-dictated mechanism, not a design choice. */
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

    .text

/* ------------------------------------------------------------------------- */
/* xtensa_switch(from=a2, to=a3): cooperative windowed swap. Called from thread   */
/* context only (arch_switch defers when arch_in_isr()). Saves `from`'s resume     */
/* state, flushes its windows, then resumes `to`. Returns -- via the windowed      */
/* underflow chain -- on the incoming thread's stack, inside its arch_switch call. */
/* ------------------------------------------------------------------------- */
    .align 4
    .global xtensa_switch
    .type xtensa_switch, @function
xtensa_switch:
    entry   a1, 32
#if KICKOS_BENCH
    /* Bench: bank the PREVIOUS switch's cost (end-start), then stamp this switch's
       start. Done here in `from`'s windowed context -- a call is safe (same as the
       trace hook); the windowed exit below cannot host a call, so the cost lands one
       switch late (irrelevant over millions of switches). a2=from/a3=to survive the
       call4 (they sit below the callee window). */
    movi    a7, g_bench_sw_end
    l32i    a7, a7, 0
    beqz    a7, .Lbench_prime         /* no previous switch yet */
    movi    a6, g_bench_sw_start
    l32i    a6, a6, 0
    sub     a6, a7, a6                /* a6 = delta = end - start (call4 arg0) */
    call4   kickos_bench_switch_done
.Lbench_prime:
    rsr.ccount a7
    movi    a6, g_bench_sw_start
    s32i    a7, a6, 0                 /* stamp this switch's start */
#endif
#if KICKOS_TELEMETRY
    /* SWITCH record, emitted before any state save while still in `from`'s windowed
       context. from=a2 / to=a3 are the definitive switch args (no shared-state
       re-read race). call4 rotates the window by 4, so caller a4/a5 become the
       callee's a0/a1 and are clobbered; a2/a3 sit below the callee window and
       survive the call, so the save below still sees them. */
    l32i    a6, a2, CTX_TID       /* from_tid -> hook arg0 (callee a2) */
    l32i    a7, a3, CTX_TID       /* to_tid   -> hook arg1 (callee a3) */
    call4   kickos_trace_switch_done
#endif
    /* Save the outgoing thread's resume state. a0 is the return address WITH its
       CALLINC field (top 2 bits) -- the resuming retw needs it to pick the right
       underflow width. a1 (our frame) is unchanged by the spill below, so record
       it now. */
    rsr.ps  a4
    s32i    a0, a2, CTX_PC        /* from->pc */
    s32i    a4, a2, CTX_PS        /* from->ps */
    s32i    a1, a2, CTX_SP        /* from->sp (this switch frame) */
    movi    a5, RESUME_COOP
    s32i    a5, a2, CTX_KIND      /* from->resume_kind = COOP (retw resume) */
    s32i    a3, a1, 0             /* stash `to` across the spill (defensive) */

    /* Flush every live window of `from` to its stack. EPC1 is dead in thread
       context (we are not inside an exception), so it need not be preserved. */
    movi    a3, PS_SPILL
    wsr.ps  a3
    rsync
    SPILL_ALL_WINDOWS

    /* Resume the incoming thread. A COOP thread (the common case) loads sp/ps/pc
       and returns into its saved continuation: the retw rotates the window back
       per to->pc's CALLINC and the underflow vector reloads the incoming caller
       from [to->sp - 16 ..]. A thread previously PREEMPTED (resume_kind==IRQ) has
       no retw continuation -- its full state is a 256-byte interrupt frame -- so it
       is resumed by the shared rfe epilogue instead. */
    l32i    a3, a1, 0             /* reload `to` */
    l32i    a5, a3, CTX_KIND      /* to->resume_kind */
    bnez    a5, .Lsw_resume_irq
    l32i    a1, a3, CTX_SP        /* a1 = to->sp   (switch stacks) */
    l32i    a4, a3, CTX_PS        /* to->ps */
    l32i    a0, a3, CTX_PC        /* to->pc (return addr, carries CALLINC) */
#if KICKOS_BENCH
    /* Stamp this switch's END (COOP path). a5/a6 are dead here (retw discards this
       window); the next switch entry banks (end - start). */
    rsr.ccount a5
    movi    a6, g_bench_sw_end
    s32i    a5, a6, 0
#endif
    wsr.ps  a4
    rsync
    retw
.Lsw_resume_irq:
    /* The window file is spilled to a single frame here (SPILL_ALL_WINDOWS above) --
       exactly the precondition _kickos_lx6_irq_restore assumes. Hand it the frame
       base in a1 and let it reload the full context and rfe. jx (not j) so the
       cross-section reach to startup.S is range-independent. */
    l32i    a1, a3, CTX_SP        /* a1 = to->sp (interrupt frame base) */
    movi    a2, _kickos_lx6_irq_restore
    jx      a2
    .size xtensa_switch, .-xtensa_switch

/* ------------------------------------------------------------------------- */
/* arch_start(boot=a2, first=a3): enter the first thread from the boot context.    */
/* `boot` is ignored and the boot stack abandoned (the system always terminates    */
/* via arch_shutdown, never by unwinding to boot). Records g_arch_current, spills   */
/* the boot windows, then resumes `first` by the path its resume_kind selects --    */
/* exactly as xtensa_switch does. A fresh thread (arch_context_init) is always      */
/* KICKOS_RESUME_IRQ, so the first thread is entered via the rfe restore.           */
/* ------------------------------------------------------------------------- */
    .align 4
    .global arch_start
    .type arch_start, @function
arch_start:
    entry   a1, 32
    movi    a4, g_arch_current
    s32i    a3, a4, 0
#if KICKOS_TELEMETRY
    /* First SWITCH record: from = 0xFFFF (no outgoing thread). a3 (first) is below
       the call4 callee window, so it survives for the stash below. */
    movi    a6, 0xFFFF
    l32i    a7, a3, CTX_TID       /* to = first->trace_tid */
    call4   kickos_trace_switch_done
#endif
    s32i    a3, a1, 0             /* stash `first` across the spill */

    movi    a4, PS_SPILL
    wsr.ps  a4
    rsync
    SPILL_ALL_WINDOWS

    l32i    a3, a1, 0
    l32i    a5, a3, CTX_KIND      /* first->resume_kind */
    bnez    a5, .Lstart_resume_irq
    l32i    a1, a3, CTX_SP       /* a1 = first->sp */
    l32i    a4, a3, CTX_PS       /* first->ps */
    l32i    a0, a3, CTX_PC       /* first->pc */
    wsr.ps  a4
    rsync
    retw
.Lstart_resume_irq:
    /* Single-frame window here (SPILL_ALL_WINDOWS) -- the precondition the shared rfe
       epilogue assumes. Hand it the interrupt-frame base and let it rfe into `first`. */
    l32i    a1, a3, CTX_SP       /* a1 = first->sp (interrupt frame base) */
    movi    a2, _kickos_lx6_irq_restore
    jx      a2
    .size arch_start, .-arch_start

/* ------------------------------------------------------------------------- */
/* _thread_trampoline: a fresh thread's first-resume landing. arch_context_init     */
/* fabricates a 256-byte interrupt frame; _kickos_lx6_irq_restore rfe's here with    */
/* PS.CALLINC=1 and the frame's a6/a7 holding entry/arg. The `entry` below is        */
/* MANDATORY (not a compiled-in accident): it opens this as a real CALL4 window      */
/* frame -- rotating a6/a7 into a2/a3 and setting WindowStart -- so the outermost    */
/* frame overflows through valid base-save-area linkage when a deep non-blocking     */
/* call chain wraps WindowBase around the whole 64-AR file. Without it the frame is  */
/* a phantom with no caller linkage and the wrap corrupts a saved PC. (Xtensa ISA,   */
/* Windowed Register Option; ref: FreeRTOS vPortTaskWrapper.) If entry returns,      */
/* route to kickos_thread_return (noreturn).                                          */
/* ------------------------------------------------------------------------- */
    .align 4
    .global _thread_trampoline
    .type _thread_trampoline, @function
_thread_trampoline:
    entry   a1, 32                /* open a real CALL4 frame: a2=entry, a3=arg */
    mov     a10, a3               /* arg -> first C argument (call8 window) */
    callx8  a2                    /* entry(arg) */
    call8   kickos_thread_return  /* noreturn */
    .size _thread_trampoline, .-_thread_trampoline
