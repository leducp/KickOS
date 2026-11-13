/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The extents the ARMv7-M trap sites (switch.S) reserve: one at the BOTTOM of a thread's
 * stack before they write through the live PSP, one at the top of that thread's per-thread
 * KERNEL block, which the syscall trap transfers to.
 *
 * AN EXTENT AND NOT A POINTER TEST. Exception entry stacks the HARDWARE frame with the
 * PRE-exception privilege, so the MPU already refuses a kernel-aimed PSP as MSTKERR. The
 * {r4-r11} block pushed BELOW that frame is written in handler mode and refused by nothing,
 * so a PSP a few words above a neighbour's region writes into it. So the bound is on the
 * room REMAINING below the PSP.
 *
 * KICKOS_KERNEL_STACKS picks between two syscall entry designs:
 *   1  THE TRANSFER. The SVC handler starts svc_trampoline on the caller's own kernel block
 *      (ctx.kernel_sp), so the dispatch is a requirement on KICKOS_KERNEL_STACK_SIZE. Every
 *      HAS_MPU armv7m chip.
 *   0  THE RED ZONE. svc_trampoline dispatches on the caller's PSP and the SVC site refuses
 *      a PSP without room for all of it. sam3x8e, which has no MPU.
 *
 * The classes, separated by WHICH STACK they land on:
 *   PENDSV  the switcher's push on the interrupted thread's stack. PendSV_Handler runs in
 *           HANDLER mode, where ARMv7-M forces SP_main, so its kernel-descent term is 0 here
 *           and MSP measures it.
 *   SVC     the syscall trap's writes on the interrupted thread's stack.
 *   SVCK    the syscall dispatch on the caller's kernel block, at KICKOS_KERNEL_STACKS 1.
 *   EXIT    the death path, below.
 *   IDLE    idle's own stack, below.
 *   MSP     the main stack, below: boot and every exception nested on it.
 *
 * tests/static/check_trap_redzone.sh re-measures the descent figures and fails when a worst
 * case exceeds what the class reserves. The measured half and the structural half stay
 * SEPARATE macros: folded into one, a growing dispatch could eat the structural allowance
 * and the gate would still report room to spare.
 *
 * ARMv7-M defines SP bits[1:0] as RAZ/WI, so a word-alignment test on a PSP is always true
 * and the guard carries no alignment leg.
 */

#ifndef KICKOS_ARCH_ARMV7M_TRAP_STACK_H
#define KICKOS_ARCH_ARMV7M_TRAP_STACK_H

/* Pure integer macros, generated, and included from startup.S, so this header stays
   assemblable. */
#if defined(__has_include) && __has_include(<kickos/board_config.h>)
#include <kickos/board_config.h>
#endif
/* -Wundef makes an undefined name in the #if below an error, and an out-of-tree consumer
   with no generated config gets the design that needs no blocks. */
#ifndef KICKOS_KERNEL_STACKS
#define KICKOS_KERNEL_STACKS 0
#endif

/* ARMv8-M Mainline's PSPLIM and MSPLIM, from the chip file's `stack_limit`. Every SP write and
 * every exception entry is checked against the live one; a handler's push through a general
 * register is not. */
#if defined(KICKOS_HAVE_STACK_LIMIT) && KICKOS_HAVE_STACK_LIMIT
#define KICKOS_ARMV7M_SPLIM 1
#else
#define KICKOS_ARMV7M_SPLIM 0
#endif
#if KICKOS_ARMV7M_SPLIM && !KICKOS_KERNEL_STACKS
#error "the stack limit is programmed only around the kernel blocks' syscall entry; the red-zone dispatch would run privileged on the limited user stack"
#endif

/* How far above a stack's base its limit sits: the {r4-r11} and {s16-s31} block
 * PendSV and the SVC fastpath push below a PSP, which no limit checks, rounded up to the
 * registers' 8-byte granule. On a kernel block the extra word is the canary. A plain integer:
 * tests/integration/gates/pspguard.cmake scrapes it. */
#define KICKOS_ARMV7M_SPLIM_ROOM 104

/* The software block PendSV and the SVC fastpath push below the live PSP, counted off the
 * emitted code:
 *   stmdb r0!, {r4-r11}         8 words                                        32
 *   vstmdbeq r0!, {s16-s31}    16 words, only on an __ARM_FP build            +64
 *
 * FRAME_MAX is a plain integer and not an expression: the gate scrapes the enforced figures
 * out of this header as immediates. arch_armv7m.cc asserts each against the registers it
 * prices, and FRAME_MAX against their sum. */
#define KICKOS_ARMV7M_TRAP_FRAME 32
#define KICKOS_ARMV7M_TRAP_FRAME_FP 64
#define KICKOS_ARMV7M_TRAP_FRAME_MAX 96

/* THE SVC CLASS, structural half: the bytes below the guarded PSP P0 that the hardware writes
 * for the thread, which no call graph sees. At KICKOS_KERNEL_STACKS 1 the SVC handler builds
 * the trampoline's frame on the kernel block and writes nothing below P0. What stands is the
 * syscall return's window, from svc_trampoline's move back onto R, the PSP the thread resumes
 * on, to its privilege drop, where a preemption stacks on R while the thread is privileged:
 *
 *   R = P0 + 32    the entry frame the return pops, 104 with an FP frame.
 *   -32            the hardware frame a device IRQ or SysTick stacks if it preempts the window,
 *                  104 with an FP frame. PRIO_DEVICE (0x30) is above PRIO_SVCALL (0xE0) and
 *                  the window runs in THREAD mode, so ARMv7-M stacks it on the PSP. Privileged,
 *                  so no MPU region bounds it. It takes the entry frame's shape: the kernel half
 *                  executes no FP instruction (rule armv7m_kernel_fp), so CONTROL.FPCA leaves
 *                  the dispatch as the return into the trampoline set it.
 *   -32            the PendSV that tail-chains behind it, 96 with an FP frame. It cannot
 *                  repeat: PendSV switches away and pops the block on resume, and a further
 *                  exception nests on the MSP, so at most ONE exception is ever stacked on a
 *                  thread's PSP.
 *
 * So the preemption lands on P0 and the window is the PendSV block: 96 at most, FRAME_MAX,
 * the fastpath arm's own push. THE STKALIGN PAD IS NOT A TERM: the entry spends 4 exactly when
 * the pre-exception SP was 4-mod-8, which puts P0 4 bytes lower and R back on that same SP,
 * where the preempting entry spends its own 4. */
#define KICKOS_ARMV7M_TRAP_NEST_SVC 96

/* The window with the dispatch inside it, which is the KICKOS_KERNEL_STACKS 0 posture: the
 * trampoline runs on R from its first instruction, so its `push {r12, lr}` adds 8, and the
 * STKALIGN pad the preempting entry spends is a term: the dispatch's own frames are
 * word-granular, so the deepest SP can be 4-mod-8 on its own.
 *
 * It is a frame term and not rounding in the depth. Here the gate compares DEPTH >= D for
 * every parity of D; folded into the depth it would compare D against a figure that already
 * spent 4 on the pad, and pass a dispatch 4 bytes past what the site can hold. */
#define KICKOS_ARMV7M_TRAP_NEST_SVC_DISPATCH 108

/* THE SVCK CLASS, structural half: what stands between the top of a kernel block (T =
 * ctx.kernel_sp) and the first byte syscall_dispatch may use, plus what a preemption puts
 * below the deepest byte it uses.
 *
 *   -16    the continuation header the SVC handler lays at T: a3 (the fifth argument, passed
 *          on the stack by AAPCS), the caller-return, the PSP the thread resumes on, and the
 *          word that keeps SP 8-aligned for the call. The trampoline's entry frame below it,
 *          at most 104, is popped before the dispatch descends, so it and the preemption
 *          below are alternatives: 16 + 104 + 96 tail-chained onto it stays inside the sum.
 *   -4     the STKALIGN pad a preempting exception spends. It does NOT cancel here: above it
 *          is a chain of compiler frames whose sub is a multiple of 4, so the deepest SP can
 *          be 4-mod-8 on its own.
 *   -104   the hardware frame a device IRQ or SysTick stacks. The dispatch runs in THREAD
 *          mode, so ARMv7-M stacks it on the PSP, which is this block.
 *   -96    the PendSV that tail-chains behind it, by the single-nesting argument above.
 *
 * The FP terms are unconditional: an FP-live caller's dispatch runs with CONTROL.FPCA set. */
#define KICKOS_ARMV7M_TRAP_NEST_SVCK 220

/* Worst-case bytes the kernel's C dispatch descends BELOW all of that, per class.
 *
 *   _PENDSV   0  handler mode uses SP_main.
 *   _SVC    640  KICKOS_KERNEL_STACKS 0 ONLY: the whole dispatch tree runs on the caller's
 *                PSP. The spawn stages its grant list on this stack and its parameter copies
 *                in the kernel instance: 9 grants measure 600 through kos_task_create on due,
 *                rounded up to 64 as the other classes' are; arch_armv7m.cc refuses more than 9.
 *                Not zeroed at
 *                KICKOS_KERNEL_STACKS 1: the gate scrapes it on every preset.
 *   _SVCK        the same dispatch on the kernel block, posture-dependent, below.
 *
 * THE PANIC REPORTER IS ON NEITHER THIS CLASS NOR _SVCK: kpanic leaves the stack it was called
 * on before it prints (kickos_panic_stack_enter, switch.S).
 *
 * tests/static/trap_redzone_indirect.txt binds every reachable indirect site to the one slot
 * that call reaches, and the gate refuses to answer while a reachable site is unbound. */
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_PENDSV 0
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVC 640

/* KICKOS_TELEMETRY is the knob: the trace emitters and arch_shutdown's ring drain are compiled
 * by nothing else, and one fleet-wide figure would make every non-telemetry board reserve
 * a block per thread slot of a tail its image does not contain.
 *
 * Off it, 768 enforced against 600 on qemu and 640 on pizero2350-amp2-n0, a task create
 * staging its grants:
 *   syscall_dispatch[48] -> task_create_call[144] -> spawn_locked[272] -> ThreadPool::alloc[16]
 *   -> task_admit[24] -> task_usage[8] -> task_object_usage[40] -> task_object_holds[48] -> ...
 * 1240 enforced against 1088 measured on qemu-telem, a create cancelled at its wait whose exit
 * ends the system through arch_shutdown's telemetry tail. Telemetry presets stage at most 9
 * spawn grants and the partition nodes 12; arch_armv7m.cc refuses more. The doorbell's
 * service body runs in handler mode on SP_main, where the MSP class prices it.
 *
 * NO FALLBACK #define: with -Wundef -Werror an image that lost KICKOS_TELEMETRY fails to
 * build, where a fallback would silently reserve the smaller figure. */
#if KICKOS_TELEMETRY
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVCK 1240
#else
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVCK 768
#endif

/* THE EXIT CLASS, structural half: NEST_SVCK's window without the 16-byte continuation
 * header, which no trampoline lays here. The backend builds the exception frame the return
 * pops at kickos_fault_stack_top, so the exit starts with sp there.
 *
 *   -4     the STKALIGN pad, which does not cancel, as in the SVCK window.
 *   -104   the hardware frame a device IRQ or SysTick stacks. The stub runs in THREAD mode.
 *   -96    the PendSV that tail-chains behind it.
 *
 * Unlike rv32imac's KICKOS_RV_TRAP_FRAME_EXIT 0, the stub is PREEMPTIBLE: exit_current
 * reschedules. */
#define KICKOS_ARMV7M_TRAP_NEST_EXIT 204

/* The measured descent of the two stubs a dying thread runs PRIVILEGED on its own stack:
 * kickos_thread_fault_exit and kickos_thread_slay_exit. The fault stub is the deeper at every
 * posture, reaching the console through kvprintf_fault, which formats
 * into KDIAG_FAULT_LINE_MAX bytes and not the 256 an ordinary kprintf gets.
 *
 * The kstacks=0 fallback, with no posture ladder: its preset, due, has no telemetry variant
 * and measures 368. Where a block IS seated they are EXITK below. */
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_EXIT 448

/* THE SAME TWO STUBS ON THE KERNEL BLOCK. kickos_fault_stack_top answers with ctx.kernel_sp,
 * so the fault redirect and the slay rebuild both land at the block TOP, discarding whatever
 * dispatch frames it held. That discard keeps the block requirement the MAX of SVCK and this
 * rather than their sum.
 *
 * It never binds: SVCK wins the block on every registered preset. Off telemetry it measures
 * 368 to 440 under an enforced 448; on telemetry 952 stands over 808, down arch_shutdown's
 * drain. */
#if KICKOS_TELEMETRY
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_EXITK 952
#else
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_EXITK 448
#endif

/* What each guarded site enforces: room below the live PSP, in bytes. Both are loaded with
 * movw, whose imm16 range covers anything the floor can hold, so a figure that outgrew the
 * encoding fails to assemble rather than truncating.
 *
 * ONLY PENDSV TAKES A RUN-TIME FP TERM. PendSV's push IS the {s16-s31} block, while
 * NEST_SVC is a structural constant that bounds both entry postures at the FP-live one's 96. */
#define KICKOS_ARMV7M_TRAP_NEED_PENDSV \
    (KICKOS_ARMV7M_TRAP_FRAME + KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_PENDSV)

/* Resolved by which entry design this build compiles, so switch.S's guard, arch_armv7m.cc's
 * floor assert, kickos_arm_bad_psp's reported count and user/apps/common/pspguard's
 * expectation are one value and not four agreements. */
#if KICKOS_KERNEL_STACKS && KICKOS_ARMV7M_SPLIM
/* Every entry stacks at or above PSPLIM, which sits SPLIM_ROOM over stack_lo and covers the
 * window, so the lower leg asks for that room and only the upper leg can refuse. */
#define KICKOS_ARMV7M_TRAP_NEED_SVC \
    (KICKOS_ARMV7M_TRAP_NEST_SVC - KICKOS_ARMV7M_TRAP_FRAME_MAX + KICKOS_ARMV7M_SPLIM_ROOM)
#elif KICKOS_KERNEL_STACKS
#define KICKOS_ARMV7M_TRAP_NEED_SVC KICKOS_ARMV7M_TRAP_NEST_SVC
#else
#define KICKOS_ARMV7M_TRAP_NEED_SVC \
    (KICKOS_ARMV7M_TRAP_NEST_SVC_DISPATCH + KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVC)
#endif

/* The exception frame the SVC entry stacks ABOVE the PSP the guard validates, which the guard
 * does not count and the thread's own stack still pays: 104 where an __ARM_FP build can enter
 * with an FP frame, and the plain 32 elsewhere. So a thread needs this plus
 * KICKOS_ARMV7M_TRAP_NEED_SVC below its deepest sp, which also covers the preemption and the
 * PendSV push, both smaller. tests/static/app_stack_roots.txt prices app threads on it. */
#if defined(__ARM_FP)
#define KICKOS_ARMV7M_TRAP_ENTRY_FRAME 104
#else
#define KICKOS_ARMV7M_TRAP_ENTRY_FRAME 32
#endif

/* What one kernel block has to hold: a requirement on KICKOS_KERNEL_STACK_SIZE, not a bound
 * anything refuses at run time, every byte of it being written by privileged code through a
 * pointer the kernel seated. It resolves per POSTURE, of which there are three, so that the
 * Kconfig ceiling, arch_armv7m.cc's static_assert and check_trap_redzone.sh all price the same
 * one. Kconfig adds the canary word and rounds to 16. */
#define KICKOS_ARMV7M_TRAP_NEED_SVCK \
    (KICKOS_ARMV7M_TRAP_NEST_SVCK + KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVCK)

/* struct arch_context field offsets the trap sites read as plain displacements. switch.S
 * .equ's from these and arch_armv7m.cc static_asserts offsetof against them. The
 * telemetry-only trace_tid is the LAST field precisely so that no build posture shifts them,
 * which would make the guard compare a PSP against a trace id; kernel_sp sits ahead of it for
 * that reason. */
#define KICKOS_ARMV7M_CTX_OFF_STACK_LO 12
#define KICKOS_ARMV7M_CTX_OFF_STACK_HI 16
#define KICKOS_ARMV7M_CTX_OFF_KERNEL_SP 20
#define KICKOS_ARMV7M_CTX_OFF_EXC_RETURN 24
#if KICKOS_ARMV7M_SPLIM
#define KICKOS_ARMV7M_CTX_OFF_SPLIM 28
#define KICKOS_ARMV7M_CTX_OFF_TRACE_TID 32
#else
#define KICKOS_ARMV7M_CTX_OFF_TRACE_TID 28
#endif

/* THE PANIC REPORTER'S OWN STACK, which kickos_panic_stack_enter (switch.S) moves to before a
 * banner is printed. The PANIC console is priced here and on no other class of this arch: SVC,
 * SVCK, EXIT and EXITK reach the reporter through a body the callgraph walk stops at. The
 * fault reporter is a different chain, on the dying thread's own stack, and EXIT and EXITK
 * measure it.
 *
 * FRAME is 0: only a fault inside the reporter stacks one here, a bug the tests catch.
 *
 * Telemetry is its own size class, as it is for the kernel block: arch_shutdown's ring drain
 * sits under kfault_terminate, inside the reporter. */
#define KICKOS_ARMV7M_PANIC_FRAME 0
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
#define KICKOS_ARMV7M_PANIC_DEPTH 832
#else
#define KICKOS_ARMV7M_PANIC_DEPTH 320
#endif

/* THE MAIN STACK'S OVERFLOW REPORT, on an array of its own where the core has stack limits:
 * kickos_armv7m_stkof (arch_armv7m.cc) moves MSP to its top and MSPLIM to its base before any
 * C runs, so the report of an MSP overflow cannot descend into the arena below the main stack.
 * Any fault taken on that array ends the system from its top, through kfault_terminate.
 *
 * The class is measured on every armv7m preset, the fault reporter being the same chain on the
 * main stack of a core without limits; only a core with them allocates the array. FRAME and
 * telemetry as for PANIC. */
#define KICKOS_ARMV7M_STKOF_FRAME 0
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
#define KICKOS_ARMV7M_STKOF_DEPTH 1024
#else
#define KICKOS_ARMV7M_STKOF_DEPTH 768
#endif

/* IDLE'S OWN STACK, its PSP, which idle_entry runs on privileged. An interrupt stacks its
 * hardware frame there and the PendSV that tail-chains behind it pushes its block below that,
 * by the single-nesting argument above:
 *
 *   -4     the STKALIGN pad, which does not cancel below compiler frames.
 *   -104   the hardware frame, 32 without __ARM_FP.
 *   -96    the PendSV block, 32 without __ARM_FP. Where the core has stack limits PSPLIM sits
 *          KICKOS_ARMV7M_SPLIM_ROOM above the base and the hardware frame must clear it, so
 *          that room stands in for the block.
 *
 * DEPTH is idle_entry and arch_idle_wait, 8 measured on every preset, rounded up to 64. */
#if defined(__ARM_FP) && KICKOS_ARMV7M_SPLIM
#define KICKOS_ARMV7M_IDLE_FRAME 212
#elif defined(__ARM_FP)
#define KICKOS_ARMV7M_IDLE_FRAME 204
#elif KICKOS_ARMV7M_SPLIM
#define KICKOS_ARMV7M_IDLE_FRAME 140
#else
#define KICKOS_ARMV7M_IDLE_FRAME 68
#endif
#define KICKOS_ARMV7M_IDLE_DEPTH 64

/* THE MAIN STACK, which the image links as _kernel_stack_size below _estack. Boot runs on it in
 * thread mode with device lines open, sched::start leaves boot's frames on it, and every
 * exception then nests below them, one per priority level (trap_redzone_roots.txt derives the
 * levels). DEPTH is boot's deepest chain plus the deepest root of each level: 1888 to 2360 off
 * telemetry, the partition nodes deepest, and 2968 on qemu-telem. check_trap_redzone.sh compares
 * FRAME plus the measurement against the stack each image links, 8 or 16 KiB on every chip.
 *
 * FRAME: four exception entries stacked on the MSP, one per level above boot, each the FP
 * frame of 104 plus the STKALIGN pad of 4, and 24 bytes of PendSV's own pushes below its entry
 * at the telemetry posture. 4 x 36 + 24 without an FPU. */
#if defined(__ARM_FP)
#define KICKOS_ARMV7M_MSP_FRAME 456
#else
#define KICKOS_ARMV7M_MSP_FRAME 168
#endif
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
#define KICKOS_ARMV7M_MSP_DEPTH 3584
#else
#define KICKOS_ARMV7M_MSP_DEPTH 3072
#endif

/*
 * The trap classes tests/static/check_trap_redzone.sh measures against the figures above,
 * one line each; tests/static/trap_redzone_roots.txt says what each option means and roots
 * every class.
 *
 * class PENDSV frame=KICKOS_ARMV7M_TRAP_FRAME_MAX depth=KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_PENDSV
 *
 * NEST_SVC_DISPATCH: the class is kstacks=0, and that posture puts the dispatch's compiler
 * frames between the trampoline and the preemption point, so the preempting entry's STKALIGN pad
 * stops cancelling. Provisioning it in the frame is what makes the depth comparison below
 * (measured <= depth) the right test for every parity.
 *
 * class SVC    frame=KICKOS_ARMV7M_TRAP_NEST_SVC_DISPATCH depth=KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVC kstacks=0
 * class SVCK   frame=KICKOS_ARMV7M_TRAP_NEST_SVCK depth=KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVCK stack=kernel
 *
 * The death path: no prologue guards these two classes.
 *
 * EXITK is the two relocating stubs wherever a block is seated. kickos_fault_stack_top answers
 * with ctx.kernel_sp, so the fault redirect and the slay rebuild both land at the block TOP.
 *
 * class EXITK  frame=KICKOS_ARMV7M_TRAP_NEST_EXIT depth=KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_EXITK stack=kernel
 *
 * EXIT is the fallback where KICKOS_KERNEL_STACKS resolves 0: both stubs stay on the thread's
 * own stack. KICKOS_KERNEL_STACKS needs the chip's HAS_MPU, which sam3x8e does not select.
 *
 * class EXIT   frame=KICKOS_ARMV7M_TRAP_NEST_EXIT depth=KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_EXIT kstacks=0
 *
 * class PANIC  frame=KICKOS_ARMV7M_PANIC_FRAME depth=KICKOS_ARMV7M_PANIC_DEPTH stack=panic
 * class IDLE   frame=KICKOS_ARMV7M_IDLE_FRAME depth=KICKOS_ARMV7M_IDLE_DEPTH stack=idle
 * class STKOF  frame=KICKOS_ARMV7M_STKOF_FRAME depth=KICKOS_ARMV7M_STKOF_DEPTH stack=trap
 * class MSP    frame=KICKOS_ARMV7M_MSP_FRAME depth=KICKOS_ARMV7M_MSP_DEPTH stack=main
 */

#endif /* KICKOS_ARCH_ARMV7M_TRAP_STACK_H */
