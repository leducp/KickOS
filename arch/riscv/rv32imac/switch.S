/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * RV32IMAC context switch + trap entry + syscall trampoline. One mtvec DIRECT-mode
 * trap (trap_entry) demuxes on mcause; mechanisms:
 *   1. trap_entry     -- saves the FULL interrupted context (all GPRs + mepc +
 *                        mstatus) on the running thread's OWN stack, then demuxes.
 *                        ONE frame format for every path (the RX single-frame
 *                        model), so a preempted thread and a blocked-in-syscall
 *                        thread resume identically.
 *   2. .Lswitch       -- the deferred switcher (mcause = machine software / msip).
 *                        arch_switch pends msip; the physical swap ALWAYS happens
 *                        here (the PendSV/SWINT analog). Saves outgoing, restores
 *                        incoming, mret. Runs with MIE=0 (atomic).
 *   3. .Lecall + svc_trampoline -- the syscall trap. ecall rewrites the return so
 *                        mret lands in svc_trampoline running M-mode (privileged)
 *                        on the calling thread's OWN stack, so a blocking dispatch's
 *                        continuation is per-thread (arch.h arch_syscall contract).
 *   4. arch_start     -- enter the first thread: no outgoing to save, jump straight
 *                        into the shared restore epilogue.
 *   5. arch_syscall   -- the userspace trap stub (args already in a0-a4).
 *
 * Save frame (128 B, low->high on the thread's stack; ctx.sp = its base). Must match
 * arch_context_init (arch_rv32imac.cc). gp/tp are NOT saved (link-time constant
 * across threads). ecall is always 4 bytes (no compressed form), so the user return
 * is mepc+4.
 */

#include <kickos/arch/rv_trap_ids.h>

    .equ F_MEPC,     0
    .equ F_MSTATUS,  4
    .equ F_RA,       8
    .equ F_T0,      12
    .equ F_T1,      16
    .equ F_T2,      20
    .equ F_S0,      24
    .equ F_S1,      28
    .equ F_A0,      32
    .equ F_A1,      36
    .equ F_A2,      40
    .equ F_A3,      44
    .equ F_A4,      48
    .equ F_A5,      52
    .equ F_A6,      56
    .equ F_A7,      60
    .equ F_S2,      64
    .equ F_S3,      68
    .equ F_S4,      72
    .equ F_S5,      76
    .equ F_S6,      80
    .equ F_S7,      84
    .equ F_S8,      88
    .equ F_S9,      92
    .equ F_S10,     96
    .equ F_S11,    100
    .equ F_T3,     104
    .equ F_T4,     108
    .equ F_T5,     112
    .equ F_T6,     116
    .equ FRAME,    128

    .text

/* ------------------------------------------------------------------------- */
/* Vectored mtvec table. The ESP32-C6 core supports ONLY vectored mtvec (TRM:  */
/* "Only vectored mode 0x1 is available"); QEMU virt supports it too, so both   */
/* chips share this. Hardware vectors an INTERRUPT of cause i to BASE + 4*i and */
/* any EXCEPTION to BASE (slot 0). Every slot jumps to the one demux handler    */
/* (trap_entry), so we satisfy the C6's vectored-only requirement while keeping */
/* the single-handler mcause demux below. Base is 256-byte aligned (required in */
/* vectored mode); each slot MUST be a 4-byte entry (BASE+4*i), so norvc.       */
/* ------------------------------------------------------------------------- */
    .balign 256
    .global kickos_rv_mtvec
kickos_rv_mtvec:
    .option push
    .option norvc
    .rept 32
    j       trap_entry
    .endr
    .option pop

/* ------------------------------------------------------------------------- */
/* trap_entry: save the full context on the interrupted stack, then demux      */
/* mcause. Reached from every mtvec vector slot. Entered with MIE=0 (hw).       */
/* ------------------------------------------------------------------------- */
    .balign 4
    .global trap_entry
trap_entry:
    addi    sp, sp, -FRAME
    sw      ra,  F_RA(sp)
    sw      t0,  F_T0(sp)
    sw      t1,  F_T1(sp)
    sw      t2,  F_T2(sp)
    sw      s0,  F_S0(sp)
    sw      s1,  F_S1(sp)
    sw      a0,  F_A0(sp)
    sw      a1,  F_A1(sp)
    sw      a2,  F_A2(sp)
    sw      a3,  F_A3(sp)
    sw      a4,  F_A4(sp)
    sw      a5,  F_A5(sp)
    sw      a6,  F_A6(sp)
    sw      a7,  F_A7(sp)
    sw      s2,  F_S2(sp)
    sw      s3,  F_S3(sp)
    sw      s4,  F_S4(sp)
    sw      s5,  F_S5(sp)
    sw      s6,  F_S6(sp)
    sw      s7,  F_S7(sp)
    sw      s8,  F_S8(sp)
    sw      s9,  F_S9(sp)
    sw      s10, F_S10(sp)
    sw      s11, F_S11(sp)
    sw      t3,  F_T3(sp)
    sw      t4,  F_T4(sp)
    sw      t5,  F_T5(sp)
    sw      t6,  F_T6(sp)
    csrr    t0, mepc
    sw      t0, F_MEPC(sp)
    csrr    t0, mstatus
    sw      t0, F_MSTATUS(sp)

    csrr    t0, mcause
    bltz    t0, .Lintr              /* mcause[31]=1 => interrupt */
    /* exception: ecall-from-U (8) or ecall-from-M (11); else a fault */
    li      t1, 8
    beq     t0, t1, .Lecall
    li      t1, 11
    beq     t0, t1, .Lecall
    j       .Lfault
.Lintr:
    andi    t0, t0, 0x1f            /* interrupt code */
    li      t1, 3
    beq     t0, t1, .Lswitch        /* machine software (msip) -> the switcher */
    li      t1, 7
    beq     t0, t1, .Ltimer         /* machine timer (mtip) */
    li      t1, 1
    beq     t0, t1, .Lssoft         /* supervisor software (ssip) -> injected IRQ (virt) */
    li      t1, KICKOS_RV_INJECT_DOORBELL_CPU_INT
    beq     t0, t1, .Lext           /* the C6 external inject doorbell (rv_trap_ids.h) */
    li      t1, KICKOS_RV_DEV_CPU_INT
    beq     t0, t1, .Lextdev        /* a real device external IRQ (C6: UART0 TX ring) */
    /* spurious: just return */
    j       .Lrestore

/* --- Timer (mtip): tick in ISR context. kickos_isr_timer re-arms/disarms      */
/* mtimecmp (clearing mtip) and may arch_switch (pends msip -> fires on mret).   */
.Ltimer:
    la      t0, g_isr_depth
    lw      t1, 0(t0)
    addi    t1, t1, 1
    sw      t1, 0(t0)
    call    kickos_isr_timer
    la      t0, g_isr_depth
    lw      t1, 0(t0)
    addi    t1, t1, -1
    sw      t1, 0(t0)
    j       .Lrestore

/* --- Supervisor software (ssip): a software-injected IRQ in ISR context. This is */
/* the arch_irq_inject test/bench channel (the sim's SIGUSR1 twin). kickos_rv_      */
/* dispatch_soft (arch .cc) clears ssip + runs the injected line's kickos_isr_irq.  */
.Lssoft:
    la      t0, g_isr_depth
    lw      t1, 0(t0)
    addi    t1, t1, 1
    sw      t1, 0(t0)
    call    kickos_rv_dispatch_soft
    la      t0, g_isr_depth
    lw      t1, 0(t0)
    addi    t1, t1, -1
    sw      t1, 0(t0)
    j       .Lrestore

/* --- External inject doorbell (a real machine interrupt via the C6 interrupt      */
/* matrix; the C6 reports mcause = the CPU interrupt ID, here 31). Same ISR-context  */
/* discipline as .Lssoft: kickos_rv_ext_dispatch EOIs the chip source, then runs the */
/* injected line's kickos_isr_irq. MIE=0 throughout the trap, so a wake's pended     */
/* msip switch fires only after mret, with g_isr_depth back to 0 (deterministic).    */
.Lext:
    la      t0, g_isr_depth
    lw      t1, 0(t0)
    addi    t1, t1, 1
    sw      t1, 0(t0)
    call    kickos_rv_ext_dispatch
    la      t0, g_isr_depth
    lw      t1, 0(t0)
    addi    t1, t1, -1
    sw      t1, 0(t0)
    j       .Lrestore

/* --- Real device external IRQ (C6: UART0 TX-empty, routed via the interrupt matrix to */
/* KICKOS_RV_DEV_CPU_INT). Same ISR-context discipline as .Lext; kickos_rv_ext_dispatch_ */
/* dev acks the level source + runs the device line's ISR (weak no-op off the C6).      */
.Lextdev:
    la      t0, g_isr_depth
    lw      t1, 0(t0)
    addi    t1, t1, 1
    sw      t1, 0(t0)
    call    kickos_rv_ext_dispatch_dev
    la      t0, g_isr_depth
    lw      t1, 0(t0)
    addi    t1, t1, -1
    sw      t1, 0(t0)
    j       .Lrestore

/* --- The deferred switcher (msip). g_arch_current is always valid here (the    */
/* first switch goes through arch_start, not this path). s0/s1 are free scratch: */
/* the outgoing thread's s0/s1 are already saved to its frame above.             */
.Lswitch:
#if KICKOS_BENCH
    /* stamp switch entry. g_bench_cycle_src != 0 -> read that MMIO counter (a core
       whose rdcycle traps, e.g. the C6, points it at CLINT MTIME); else rdcycle. */
    la      t0, g_bench_cycle_src
    lw      t0, 0(t0)
    beqz    t0, 6f
    lw      t0, 0(t0)
    j       7f
6:  rdcycle t0
7:  la      t1, g_bench_sw_start
    sw      t0, 0(t1)
#endif
    la      t0, g_clint_msip
    lw      t0, 0(t0)
    sw      zero, 0(t0)             /* clear msip (else it re-fires immediately) */
    la      t0, g_arch_current
    lw      t1, 0(t0)               /* t1 = outgoing ctx* */
    sw      sp, 0(t1)               /* outgoing->sp = sp (frame just saved) */
#if KICKOS_TELEMETRY
    lw      s0, 4(t1)               /* from_tid = outgoing->trace_tid */
#endif
    la      t2, g_arch_next
    lw      t3, 0(t2)               /* t3 = incoming ctx* */
    sw      t3, 0(t0)               /* g_arch_current = incoming */
    lw      sp, 0(t3)               /* sp = incoming->sp */
#if KICKOS_TELEMETRY
    lw      s1, 4(t3)               /* to_tid = incoming->trace_tid */
#endif
#if KICKOS_BENCH
    la      t0, g_bench_cycle_src   /* delta = now - entry -> bench (before telemetry) */
    lw      t0, 0(t0)
    beqz    t0, 8f
    lw      t0, 0(t0)
    j       9f
8:  rdcycle t0
9:  la      t1, g_bench_sw_start
    lw      t1, 0(t1)
    sub     a0, t0, t1
    call    kickos_bench_switch_done
#endif
#if KICKOS_TELEMETRY
    mv      a0, s0
    mv      a1, s1
    call    kickos_trace_switch_done   /* (from,to) from the swapped contexts */
#endif
    j       .Lrestore

/* --- ecall: redirect mret into svc_trampoline running M-mode (privileged) on  */
/* the caller's own stack. Args are already in the frame's a0-a4 slots. The      */
/* frame keeps the USER mstatus (MPP=caller priv) for the eventual return.       */
.Lecall:
    lw      a0, F_A0(sp)            /* nr */
    lw      a1, F_A1(sp)
    lw      a2, F_A2(sp)
    lw      a3, F_A3(sp)
    lw      a4, F_A4(sp)
    la      t0, svc_trampoline
    csrw    mepc, t0
    csrr    t0, mstatus
    li      t1, 0x1800             /* MPP = M (11): mret -> M-mode trampoline */
    or      t0, t0, t1
    li      t1, 0x80               /* MPIE = 1: MIE on in the trampoline */
    or      t0, t0, t1
    csrw    mstatus, t0
    mret                            /* -> svc_trampoline; sp=frame, a0-a4=args */

/* Runs M-mode (privileged) on the calling thread's stack, sp = its saved frame. */
/* A blocking syscall_dispatch here blocks by an ordinary arch_switch (pends     */
/* msip); the msip switcher freezes the mid-dispatch continuation on THIS stack  */
/* and resumes it inline when the thread is rescheduled. sp (callee-saved)       */
/* survives the call, so it still points at the frame afterwards.                */
    .global svc_trampoline
svc_trampoline:
    call    syscall_dispatch        /* (a0..a4) -> result in a0 */
    sw      a0, F_A0(sp)            /* frame.a0 = result */
    lw      t0, F_MEPC(sp)          /* ecall pc */
    addi    t0, t0, 4               /* -> instruction after ecall */
    sw      t0, F_MEPC(sp)
    j       .Lrestore               /* restore frame -> mret to user (MPP=caller) */

/* --- Shared restore epilogue: pop the frame at sp, mret. (t0 is scratch for    */
/* the CSR loads, then reloaded with the rest.)                                  */
.Lrestore:
    /* Restore mstatus (saved with MIE=0) BEFORE mepc. The ecall-return path reaches
       here from svc_trampoline with MIE=1 (.Lecall set MPIE=1); a trap taken between
       the mepc and mstatus writes would clobber mepc (the nested trap overwrites it),
       and the final mret would then jump back into this epilogue at the caller's
       privilege with the frame half-popped. Writing mstatus first (MIE->0) closes
       that window before mepc is set. Other entries reach here with MIE already 0. */
    lw      t0, F_MSTATUS(sp)
    csrw    mstatus, t0
    lw      t0, F_MEPC(sp)
    csrw    mepc, t0
    lw      ra,  F_RA(sp)
    lw      t0,  F_T0(sp)
    lw      t1,  F_T1(sp)
    lw      t2,  F_T2(sp)
    lw      s0,  F_S0(sp)
    lw      s1,  F_S1(sp)
    lw      a0,  F_A0(sp)
    lw      a1,  F_A1(sp)
    lw      a2,  F_A2(sp)
    lw      a3,  F_A3(sp)
    lw      a4,  F_A4(sp)
    lw      a5,  F_A5(sp)
    lw      a6,  F_A6(sp)
    lw      a7,  F_A7(sp)
    lw      s2,  F_S2(sp)
    lw      s3,  F_S3(sp)
    lw      s4,  F_S4(sp)
    lw      s5,  F_S5(sp)
    lw      s6,  F_S6(sp)
    lw      s7,  F_S7(sp)
    lw      s8,  F_S8(sp)
    lw      s9,  F_S9(sp)
    lw      s10, F_S10(sp)
    lw      s11, F_S11(sp)
    lw      t3,  F_T3(sp)
    lw      t4,  F_T4(sp)
    lw      t5,  F_T5(sp)
    lw      t6,  F_T6(sp)
    addi    sp, sp, FRAME
    mret

/* --- Unhandled exception: dump the trap CSRs + hand off to the shared dead-end   */
/* (a fault is a genuine bug at M1). mcause/mepc/mtval/mstatus are still live -- no */
/* trap has been taken since the vector -- so read them straight into the args. The */
/* saved frame is at sp (a valid stack for the call). kickos_rv_fault_report is      */
/* [[noreturn]] (blink on real HW, exit on QEMU); the spin is a belt-and-braces stop. */
.Lfault:
    csrr    a0, mcause
    csrr    a1, mepc
    csrr    a2, mtval
    csrr    a3, mstatus
    call    kickos_rv_fault_report
1:  j       1b

/* ------------------------------------------------------------------------- */
/* arch_start(boot, first): a0=boot (ignored), a1=first. No outgoing to save;   */
/* seed g_arch_current and enter the first thread via the shared restore. The    */
/* fabricated frame's mstatus (MPIE=1) turns MIE on -- releasing the crit level  */
/* the scheduler held. Never returns (boot stack abandoned).                     */
/* ------------------------------------------------------------------------- */
    .global arch_start
arch_start:
    la      t0, g_arch_current
    sw      a1, 0(t0)               /* g_arch_current = first */
    mv      t0, a1                  /* preserve first ctx* across the sp load */
    lw      sp, 0(t0)               /* sp = first->sp (fabricated frame) */
#if KICKOS_TELEMETRY
    lw      a1, 4(t0)               /* to_tid = first->trace_tid */
    li      a0, 0xFFFF              /* from = no-thread (decoder's first-switch) */
    call    kickos_trace_switch_done   /* sp (callee-saved) survives */
#endif
    j       .Lrestore

/* ------------------------------------------------------------------------- */
/* arch_syscall(nr, a0, a1, a2, a3): args already in a0-a4 per the psABI; ecall  */
/* traps to .Lecall, svc_trampoline runs dispatch and returns here (post-ecall)  */
/* with a0 = result. ra is preserved across the trap (saved/restored in frame).  */
/* ------------------------------------------------------------------------- */
    .global arch_syscall
arch_syscall:
    ecall
    ret
