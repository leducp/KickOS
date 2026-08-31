/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * RV32IMAC context switch, trap entry and syscall trampoline. Every mtvec slot enters
 * trap_entry, which saves the FULL interrupted context (every GPR bar gp/tp, plus mepc and
 * mstatus) and demuxes on mcause. ONE frame format for every path, so a preempted thread and
 * a blocked-in-syscall thread resume identically.
 *   .Lswitch     the deferred switcher (mcause = msip). arch_switch pends msip; the
 *                physical swap ALWAYS happens here, with MIE=0.
 *   .Lecall      the syscall trap. mret lands in svc_trampoline running M-mode on the
 *                CALLING THREAD'S kernel stack from U-mode, or on the sp it interrupted
 *                when the caller was already M-mode, so a blocking dispatch's continuation
 *                is per-thread either way (arch.h arch_syscall contract).
 *   arch_start   enters the first thread through the shared restore epilogue.
 *
 * WHICH STACK A FRAME GOES ON, three destinations and no fourth. A U-mode entry puts the
 * frame on the interrupted thread's own KERNEL stack (ctx.kernel_sp), so the sp the thread
 * chose is never written through. The two M-mode causes whose frame the switcher stores as
 * outgoing->sp (msip, ecall-from-M) keep the interrupted sp, which is that kernel stack when
 * a dispatch was interrupted and a PRIVILEGED thread's own stack when one was running. Every
 * other M-mode trap pops its frame and mrets, so its frame and the C below it go on the
 * trusted per-hart trap stack. The frame's F_SP slot carries the sp .Lrestore leaves on,
 * which is what lets one epilogue serve all three.
 *
 * MSCRATCH INVARIANT: outside the prologue, mscratch holds the trap-stack TOP. That is what
 * makes trap_entry's first instruction a swap onto a trusted sp, and it holds through the
 * U-mode accept path (which writes it back rather than swapping), through the nested path
 * and through .Ltrap_wild.
 *
 * Save frame (128 B, low->high; ctx.sp = its base). Must match arch_context_init
 * (arch_rv32imac.cc). gp and tp stay OUT of the frame and are written from the kernel's own
 * knowledge on every resume instead: gp from the link-time __global_pointer$, tp from the
 * running context's stack_lo. Both are U-mode writable, so a saved copy would be a value
 * the outgoing thread chose. ecall is always 4 bytes (no compressed form), so the user
 * return is mepc+4.
 */

#include <kickos/arch/rv_trap_ids.h>
#include <kickos/arch/rv_trap_stack.h>

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
    .equ F_SP,     KICKOS_RV_TRAP_F_SP
    .equ FRAME,    128
    .if FRAME != KICKOS_RV_TRAP_FRAME
    .error "FRAME disagrees with KICKOS_RV_TRAP_FRAME (rv_trap_stack.h)"
    .endif
    .if F_S2 != KICKOS_RV_TRAP_F_S2
    .error "F_S2 disagrees with KICKOS_RV_TRAP_F_S2 (the faultsurvive kwrite arm aims at it)"
    .endif
    .if F_SP <= F_T6
    .error "F_SP overlaps a saved GPR slot"
    .endif
    .if F_SP > FRAME - 4
    .error "F_SP lies outside the frame"
    .endif

/* struct arch_context field offsets read by trap_entry's U-mode entry. */
    .equ F_CTX_STACK_LO, KICKOS_RV_CTX_OFF_STACK_LO
    .equ F_CTX_STACK_HI, KICKOS_RV_CTX_OFF_STACK_HI
    .equ F_CTX_KERNEL_SP, KICKOS_RV_CTX_OFF_KERNEL_SP

    .text

/* Vectored mtvec table. The ESP32-C6 core supports ONLY vectored mtvec (TRM: "Only  */
/* vectored mode 0x1 is available"). Hardware vectors an INTERRUPT of cause i to      */
/* BASE + 4*i and any EXCEPTION to BASE, so the base must be 256-byte aligned and     */
/* each slot must be a 4-byte entry, hence norvc.                                     */
    .balign 256
    .global kickos_rv_mtvec
kickos_rv_mtvec:
    .option push
    .option norvc
    .rept 32
    j       trap_entry
    .endr
    .option pop

/* trap_entry: save the full context, then demux mcause. Entered with MIE=0. */
    .balign 4
    .global trap_entry
trap_entry:
    /* Swap onto the trusted per-hart trap stack before touching the interrupted sp. A
       U-mode thread owns its sp and can aim it at kernel memory, and the prologue below
       stores 28 GPRs plus mepc and mstatus through it in M-mode, which bypasses the
       unlocked PMP entries. After this swap sp is trusted and mscratch holds the
       interrupted sp. */
    csrrw   sp, mscratch, sp
    sw      t0, -4(sp)              /* stash two scratch regs on the trusted stack */
    sw      t1, -8(sp)
    csrr    t0, mstatus
    srli    t1, t0, 11             /* MPP is mstatus[12:11]; 0x1800 exceeds andi's imm */
    andi    t1, t1, 3
    bnez    t1, .Ltrap_from_m      /* MPP nonzero => interrupted M-mode (trusted sp) */
    /* Interrupted U-mode: the interrupted sp is checked for the RETURN. No frame is built
       through it, but .Lrestore loads it out of F_SP and mret resumes U-mode on it, so an sp
       outside the thread's own stack or misaligned is refused here rather than handed back.
       THE INVARIANT IS RESTORED BEFORE THE TESTS AND NOT AFTER THE FRAME IS BUILT. MIE=0
       masks interrupts and not synchronous exceptions, so a fault on any access below would
       re-enter trap_entry, whose first instruction swaps sp with mscratch and would land the
       nested handler on the U-mode sp. Nothing faultable stands between the csrr and the
       csrw, so mscratch holds a thread's sp for no access at all. */
    csrr    t1, mscratch           /* t1 = interrupted user sp */
    csrw    mscratch, sp           /* mscratch = trap top: invariant restored */
    andi    t0, t1, KICKOS_RV_TRAP_SP_ALIGN - 1
    bnez    t0, .Ltrap_wild        /* misaligned sp */
    la      t0, g_arch_current
    lw      t0, 0(t0)              /* t0 = current ctx* (the running thread) */
    lw      t0, F_CTX_STACK_HI(t0)
    bgtu    t1, t0, .Ltrap_wild    /* sp > stack_hi -> wild */
    la      t0, g_arch_current
    lw      t0, 0(t0)
    lw      t0, F_CTX_STACK_LO(t0)
    bltu    t1, t0, .Ltrap_wild    /* sp < stack_lo -> wild */
    /* Accepted: the frame and the dispatch below it go on the thread's own kernel stack. sp
       is still the trap top here, t1 carries the sp to hand back, and the originals of t0/t1
       are at -4(sp)/-8(sp). */
    la      t0, g_arch_current
    lw      t0, 0(t0)
    lw      t0, F_CTX_KERNEL_SP(t0)
    beqz    t0, .Ltrap_nokstack    /* a U-mode thread with no kernel stack */
    addi    t0, t0, -FRAME         /* frame base on the kernel stack */
    sw      ra, F_RA(t0)           /* ra's original into the frame; ra is now scratch */
    sw      t1, F_SP(t0)           /* the sp .Lrestore mrets on */
    lw      ra, -4(sp)             /* ra = t0's original, staged */
    lw      t1, -8(sp)
    mv      sp, t0
    mv      t0, ra
    j       .Ltrap_regs            /* .Ltrap_regs stores no ra: F_RA is written above */
.Ltrap_from_m:
    /* Interrupted M-mode, so the sp is the kernel's and needs no bounds check. It still
       needs a DECISION: two of these causes produce a frame that IS a thread's saved context
       and so must stay on the sp it interrupted, msip and ecall-from-M. A privileged
       thread's syscall therefore descends on its own stack. Every other M-mode trap pops its
       frame and mrets, so it takes the trusted trap stack. */
    csrr    t0, mcause
    li      t1, KICKOS_RV_MCAUSE_MSIP
    beq     t0, t1, .Ltrap_from_m_ctx
    li      t1, 11                 /* ecall from M */
    beq     t0, t1, .Ltrap_from_m_ctx
    lw      t1, -8(sp)
    lw      t0, -4(sp)
    j       .Ltrap_nested          /* sp stays the trap-stack top */
.Ltrap_from_m_ctx:
    lw      t1, -8(sp)
    lw      t0, -4(sp)
    csrrw   sp, mscratch, sp       /* sp = interrupted kernel sp; mscratch = trap top */
    j       .Ltrap_build
/* Reached only from .Ltrap_from_m_ctx, sp already swapped to the interrupted kernel sp. A
   label of its own for F_SP alone: this frame is resumed on the stack it sits on, so F_SP is
   its own base plus FRAME, where .Ltrap_nested's names a different stack. ra is free scratch
   from the moment its own slot is written. */
.Ltrap_build:
    addi    sp, sp, -FRAME
    sw      ra,  F_RA(sp)
    addi    ra, sp, FRAME
    sw      ra,  F_SP(sp)
    j       .Ltrap_regs
/* The frame goes on the trusted trap stack and the interrupted sp is only remembered, never
   written through. mscratch holds the interrupted sp from trap_entry's swap until the csrrw
   below, a window where a trap would swap onto the interrupted sp instead of the trap top.
   MIE=0 does NOT close it, masking interrupts and not synchronous exceptions; what closes it
   is that every memory access in between is an aligned word on the trap stack.
   F_SP here IS the -8(sp) slot trap_entry stashed t1 in, this frame starting one FRAME below
   the trap-stack top. Sound only because .Ltrap_from_m has already reloaded that word into
   t1; do not move the reload after this store. */
.Ltrap_nested:
    addi    sp, sp, -FRAME
    sw      ra,  F_RA(sp)
    addi    ra, sp, FRAME
    csrrw   ra, mscratch, ra       /* mscratch = trap top; ra = interrupted sp */
    sw      ra,  F_SP(sp)
.Ltrap_regs:
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
#if defined(KICKOS_ENABLE_SELFTEST) && !KICKOS_BENCH
    /* Sited after msip is out: that frame is a saved context and belongs on the thread's
       stack. mcause is re-read below because t0/t1 do not survive the call. */
    mv      a0, sp
    call    kickos_rv_nested_witness
    csrr    t0, mcause
    andi    t0, t0, 0x1f
#endif
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

/* --- Timer (mtip), ISR context. kickos_isr_timer re-arms or disarms mtimecmp,  */
/* which is what clears mtip, and may arch_switch, whose msip fires on mret.      */
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

/* --- Supervisor software (ssip): the arch_irq_inject test/bench channel, ISR      */
/* context. kickos_rv_dispatch_soft clears ssip before running the line's ISR.      */
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

/* --- External inject doorbell: a real machine interrupt through the C6 interrupt  */
/* matrix, the C6 reporting mcause = the CPU interrupt ID, here 31. MIE=0 throughout */
/* the trap, so a wake's pended msip switch fires only after mret, g_isr_depth back  */
/* to 0.                                                                             */
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

/* --- Real device external IRQ (C6: UART0 TX-empty, on KICKOS_RV_DEV_CPU_INT), ISR     */
/* context. kickos_rv_ext_dispatch_dev acks the LEVEL source before running the ISR.     */
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

/* --- The deferred switcher (msip). g_arch_current is always valid here, the    */
/* first switch going through arch_start. s0/s1 are free scratch, the outgoing    */
/* thread's copies being already in its frame.                                    */
.Lswitch:
#if KICKOS_BENCH
    /* g_bench_cycle_src != 0 selects that MMIO counter, for a core whose rdcycle traps. */
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
    /* Deferred PMP commit, after the physical swap so it is atomic with it.
       Runs with MIE=0, before .Lrestore pops the frame
       and mret drops to U-mode. sp is callee-saved and every caller-saved clobber is
       reloaded by .Lrestore next. */
    call    kickos_arch_mpu_commit
    j       .Lrestore

/* --- ecall: redirect mret into svc_trampoline, running M-mode on the stack the */
/* entry put the frame on. The frame keeps the USER mstatus (MPP = caller priv)   */
/* for the eventual return.                                                      */
.Lecall:
    /* THE SEAM. KOS_SYS_CALL_REG completes here, before the exception return into
       svc_trampoline. The leaf decides before it mutates, so a null return means nothing was
       committed. MIE is 0 from the vector, which is the mask the leaf's IPC work needs, so
       it takes no IrqLock. g_isr_depth stays put: the leaf calls scheduler code that must
       read arch_in_isr() false. */
    lw      t0, F_A0(sp)
    li      t1, 56                  /* KOS_SYS_CALL_REG (sys/abi.h) */
    bne     t0, t1, .Lecall_slow
    addi    a0, sp, F_A0            /* the saved a0-a7, contiguous and writable */
    call    kickos_ipc_fastpath
    beqz    a0, .Lecall_slow        /* refused: nothing committed, take the trampoline */
    /* Taken. a0 = the incoming thread's arch_context*. The caller is parked on its own frame
       with mepc past the ecall, so its resume is a plain .Lrestore and its result is stored
       into the frame by the switch that resumes it (sched.cc switch_book). */
    lw      t0, F_MEPC(sp)
    addi    t0, t0, 4
    sw      t0, F_MEPC(sp)
    la      t1, g_arch_current
    lw      t2, 0(t1)               /* t2 = outgoing ctx* (the caller) */
    sw      sp, 0(t2)               /* outgoing->sp = this frame */
    sw      a0, 0(t1)               /* g_arch_current = incoming */
    lw      sp, 0(a0)               /* sp = incoming->sp */
#if KICKOS_TELEMETRY
    /* The decoder checks from == previous.to, so every physically swapped pair must emit
       one. */
    lw      a1, 4(a0)               /* to_tid = incoming->trace_tid */
    lw      a0, 4(t2)               /* from_tid = outgoing->trace_tid */
    call    kickos_trace_switch_done
#endif
    /* Deferred PMP commit, the same seam as .Lswitch. */
    call    kickos_arch_mpu_commit
    j       .Lrestore
.Lecall_slow:
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

/* Runs M-mode with sp = the saved frame: the calling thread's KERNEL stack for a  */
/* U-mode caller, and the sp the trap interrupted for an M-mode one. A blocking    */
/* syscall_dispatch here blocks by an ordinary arch_switch, and the msip switcher  */
/* freezes the mid-dispatch continuation on THIS stack, resuming it inline. sp is  */
/* callee-saved, so it still points at the frame afterwards.                       */
    .global svc_trampoline
svc_trampoline:
    call    syscall_dispatch        /* (a0..a4) -> result in a0:a1 */
    /* BOTH halves: the restore epilogue reloads a0-a4 from the frame, so a result half left
       only in a register is discarded by mret. */
    sw      a0, F_A0(sp)            /* frame.a0 = result low */
    sw      a1, F_A1(sp)            /* frame.a1 = result high */
    lw      t0, F_MEPC(sp)          /* ecall pc */
    addi    t0, t0, 4               /* -> instruction after ecall */
    sw      t0, F_MEPC(sp)
    j       .Lrestore               /* restore frame -> mret to user (MPP=caller) */

/* --- Shared restore epilogue: pop the frame at sp, mret.                       */
.Lrestore:
    /* Force gp to its canonical link-time value. __global_pointer$ is anchored inside
       the LIVE app small-data window, so a U-mode thread can write gp arbitrarily and
       the next thread's gp-relative loads would resolve at addresses it chose. Every
       dispatch path funnels here, so gp is self-healing per switch. norelax: the
       assembler would otherwise relax the `la` into a no-op that assumes gp already
       holds it. */
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop
#if defined(KICKOS_TLS) && KICKOS_TLS
    /* tp = the running thread's TLS block base. RISC-V TLS is variant 2 (TP_OFFSET 0), so tp
       IS the block start, which is the thread's stack block base.
       DERIVED FROM ctx.stack_lo AND NEVER FROM THE INCOMING tp, which is U-mode writable.
       The frame's sp is no substitute either: a U-mode entry builds the frame on
       ctx.kernel_sp and F_SP carries a mid-dispatch kernel sp for a thread that blocked
       inside a syscall, so neither word names this thread's own stack on every path here.
       NO -1 CORRECTION IS DUE, unlike the ARM leaf that masks a live SP: arch_context_init
       sets stack_lo to base + block, strictly INSIDE the block, where only an exclusive
       stack TOP sits at base + stride.
       Shifts and not andi: KICKOS_TLS_STRIDE - 1 exceeds the 12-bit signed immediate.
       Idle's stack is below one stride and takes no carve, so this hands it an address in a
       neighbour's block; its body is arch_idle_wait alone and reaches no thread_local.
       KEEP THIS AFTER THE gp ANCHOR ABOVE: relaxation is back on here and the linker turns
       the `la` into `addi tp, gp, <off>`, which reads the gp those instructions just set. */
    la      tp, g_arch_current
    lw      tp, 0(tp)
    lw      tp, F_CTX_STACK_LO(tp)
    srli    tp, tp, KICKOS_TLS_STRIDE_LOG2
    slli    tp, tp, KICKOS_TLS_STRIDE_LOG2
#endif
    /* Restore mstatus (saved with MIE=0) BEFORE mepc. The ecall-return path reaches here
       from svc_trampoline with MIE=1, and a trap taken between the two writes would
       overwrite mepc, so the final mret would jump back into this epilogue at the caller's
       privilege with the frame half-popped. Writing mstatus first closes that window. */
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
    /* The frame names the sp to leave on. Last, because every load above reads through the
       frame base. */
    lw      sp,  F_SP(sp)
    mret

/* A U-mode thread with no kernel stack seated. NOT CONTAINED, and the split from .Ltrap_wild
   below is the reason: containment rebuilds the slain thread onto ctx.kernel_sp, and with no
   block there arch_ctx_redirect falls back to fabricating that privileged frame on the
   thread's own USER stack, which is the write the block design exists to refuse. It is a
   provisioning bug and not a pointer the thread chose, so it panics. */
.Ltrap_nokstack:
    csrw    mscratch, sp
    j       .Lfault

/* A U-mode trap the entry refused: an sp outside the thread's stack, or misaligned, so mret
   could not return on it. Nothing was stored through the interrupted sp. Every path here has
   already restored mscratch, so the csrw below rewrites the value that is there and holds the
   invariant for a reader arriving without following one of the three branches.

   t1 IS THE REFUSED SP and sp is the trap top, so the call below spends the trap stack and
   not the pointer under test. Both t0/t1 originals sit at -4(sp)/-8(sp) and the call
   overwrites them, which costs nothing: a contained thread's registers are discarded, and
   .Lfault reads none of them.

   .Lfault, when containment declines: the reporter's frame-trusted test refuses this
   trap-stack frame and panics, except a U-mode access fault (1/5/7), which it credits to the
   thread. This is the one path reaching .Lfault with an ECALL cause, which is why the
   reporter's cause ladder handles mcause 8. */
.Ltrap_wild:
    csrw    mscratch, sp
    mv      a0, t1
    call    kickos_rv_contain_wild_sp
    beqz    a0, .Lfault
    /* Contained: a0 names the thread to run instead. The offending thread keeps a ctx.sp the
       skipped save never wrote, and switch_book's rebuild on the way back IN is what discards
       it. */
    la      t0, g_arch_current
    lw      t1, 0(t0)              /* outgoing ctx* = the offender, read before the swap */
#if KICKOS_TELEMETRY
    /* The swap is physical, so it emits: kicktrace.py's verify() requires each SWITCH `from`
       to equal the previous `to`, and a silent swap breaks the NEXT record. s0/s1 survive the
       call, and .Lrestore reloads every caller-saved register from the frame. */
    lw      s0, 4(t1)              /* from_tid = the offender's trace_tid */
    lw      s1, 4(a0)              /* to_tid */
#endif
    sw      a0, 0(t0)
    /* The booked switch this consumes. A msip left pending would re-enter .Lswitch for a swap
       that has already happened. */
    la      t0, g_clint_msip
    lw      t0, 0(t0)
    sw      zero, 0(t0)
    lw      sp, 0(a0)              /* the incoming frame, so the commit pushes under it */
#if KICKOS_TELEMETRY
    mv      a0, s0                 /* a0 is dead once sp and g_arch_current are set */
    mv      a1, s1
    call    kickos_trace_switch_done
#endif
    call    kickos_arch_mpu_commit
    j       .Lrestore

/* --- Unhandled exception. mcause/mepc/mtval/mstatus are still live, no trap having  */
/* been taken since the vector. A NON-ZERO return means fault isolation claimed this  */
/* fault and mepc/mstatus already point at kickos_thread_fault_exit, so mret rather   */
/* than dump; on zero the reporter never returns and the spin is a backstop.          */
/* On the isolation path sp moves to the TOP of the dying thread's stack so           */
/* exit_current, cap_teardown and the fault print get the whole stack under them. It  */
/* happens HERE because the C seam's own frame lives on the very sp being replaced.   */
/* Zero means no stack to name, and sp stays on the frame.                            */
.Lfault:
    csrr    a0, mcause
    csrr    a1, mepc
    csrr    a2, mtval
    csrr    a3, mstatus
    mv      a4, sp
    call    kickos_rv_fault_report
    bnez    a0, 2f
1:  j       1b
2:  call    kickos_fault_stack_top
    beqz    a0, 3f
    andi    a0, a0, -16             /* psABI: sp is 16-byte aligned at a call boundary */
    mv      sp, a0
3:  mret

/* arch_start(boot, first): a0=boot (ignored), a1=first. The fabricated frame's  */
/* mstatus (MPIE=1) turns MIE on, releasing the crit level the scheduler held.   */
/* Never returns; the boot stack is abandoned.                                   */
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
    /* Deferred PMP commit for the first thread, the same seam as .Lswitch. MIE is still 0
       here; the fabricated mstatus enables it on the mret. */
    call    kickos_arch_mpu_commit
    j       .Lrestore

/* arch_syscall(nr, a0, a1, a2, a3): args already in a0-a4 per the psABI. ra     */
/* survives the trap in the frame. arch_syscall64 is the SAME trap: a0:a1 is the */
/* psABI's 64-bit pair.                                                          */
    .global arch_syscall
    .global arch_syscall64
arch_syscall:
arch_syscall64:
    ecall
    ret

/* int32_t arch_syscall_reg(uint32_t* io): the register-carrying IPC trap.      */
/* io[0..7] load a0-a7; on return io[1..5] take back a1-a5 and a0 is the result. */
/* t0 holds `io` across the ecall because the trap frame saves and restores it.  */
    .global arch_syscall_reg
arch_syscall_reg:
    mv      t0, a0
    lw      a0,  0(t0)
    lw      a1,  4(t0)
    lw      a2,  8(t0)
    lw      a3, 12(t0)
    lw      a4, 16(t0)
    lw      a5, 20(t0)
    lw      a6, 24(t0)
    lw      a7, 28(t0)
    ecall
    sw      a1,  4(t0)
    sw      a2,  8(t0)
    sw      a3, 12(t0)
    sw      a4, 16(t0)
    sw      a5, 20(t0)
    ret
