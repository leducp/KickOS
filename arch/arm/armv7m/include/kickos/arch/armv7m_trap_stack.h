/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The extents the ARMv7-M trap sites (switch.S) reserve, one at the BOTTOM of a thread's
 * stack before they agree to write through the live PSP, one at the top of that thread's
 * per-thread KERNEL block, which the syscall trap transfers to.
 *
 * WHY AN EXTENT AND NOT A POINTER TEST. Exception entry stacks the HARDWARE frame ABOVE the
 * PSP with the PRE-exception privilege, so the MPU refuses a kernel-aimed PSP as MSTKERR
 * before any handler runs. The {r4-r11, EXC_RETURN} block PendSV and the SVC fastpath push
 * BELOW that frame is written in handler mode and is refused by nothing, and the last word of
 * it is the EXC_RETURN the resume branches through: a neighbour that owns the word under the
 * stack and rewrites it to 0xFFFFFFF1 resumes the victim in handler mode, privileged. So the
 * bound is on the room REMAINING below the PSP.
 *
 * THIS ARCH COMPILES TWO SYSCALL ENTRY DESIGNS, and KICKOS_KERNEL_STACKS picks which.
 *
 *   AT 1, THE TRANSFER. svc_trampoline arrives privileged in THREAD mode and its first act
 *   is to relocate SP onto the caller's own per-thread kernel block (ctx.kernel_sp), so the
 *   dispatch is a requirement on KICKOS_KERNEL_STACK_SIZE and nothing privileged descends on
 *   a pointer a thread chose. Every HAS_MPU armv7m chip, which is where the transfer IS the
 *   isolation.
 *
 *   AT 0, THE RED ZONE. svc_trampoline runs the dispatch on the caller's own PSP and the SVC
 *   site refuses a PSP without room for all of it. Reached by the armv7m chips with no MPU
 *   that did not take BOARD_TAKES_KERNEL_STACKS, which today is stm32f302 and sam3x8e: with
 *   no privilege boundary the transfer buys robustness rather than isolation, and the red
 *   zone buys the same robustness without KICKOS_THREAD_SLOTS blocks of kernel .bss.
 *
 * FOUR CLASSES, and WHICH STACK separates them:
 *
 *   PENDSV  the switcher's push, on the interrupted thread's stack. PendSV_Handler runs in
 *           HANDLER mode, where ARMv7-M forces SP_main, so every bl in it
 *           (kickos_arch_mpu_commit, the telemetry and bench hooks) descends on the MSP. Its
 *           kernel-descent term is therefore 0 by handler mode rather than by measurement.
 *           Both designs.
 *
 *   SVC     the syscall trap's writes on the interrupted thread's stack. At
 *           KICKOS_KERNEL_STACKS 1 that is the fastpath arm's callee block, which IS the
 *           leaf's argument array, plus the eight scratch bytes svc_trampoline spends before
 *           it reaches ctx.kernel_sp. At 0 it is those plus the whole dispatch, which is
 *           KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVC. The fastpath arm is handler-mode either
 *           way, so its kickos_ipc_fastpath call descends on the MSP.
 *
 *   SVCK    the syscall dispatch on the caller's kernel block, at KICKOS_KERNEL_STACKS 1.
 *
 * tests/static/check_trap_redzone.sh re-measures the descent figures below and fails when a
 * worst case exceeds what the class reserves. The measured half and the structural half stay
 * SEPARATE macros: folded into one, a growing dispatch could eat the structural allowance and
 * the gate would still report room to spare.
 *
 * ARMv7-M defines SP bits[1:0] as RAZ/WI, so a word-alignment test on a PSP is always true
 * and the guard carries no alignment leg. user/apps/common/pspguard writes an SP with both
 * bits set, reads it back, and reports what the core kept.
 */

#ifndef KICKOS_ARCH_ARMV7M_TRAP_STACK_H
#define KICKOS_ARCH_ARMV7M_TRAP_STACK_H

/* KICKOS_KERNEL_STACKS, which selects between the two entry designs below. Pure integer
   macros, generated, and already included from startup.S, so this header stays assemblable.
   Guarded the way kernel/include/kickos/config/system.h guards it: absent, the ladder below
   takes its 0 branch, which is the design that needs no blocks. */
#if defined(__has_include) && __has_include(<kickos/board_config.h>)
#include <kickos/board_config.h>
#endif
/* An out-of-tree consumer with no generated config gets the design that needs no blocks,
   which is also what -Wundef needs: an undefined name in the #if below is an error. */
#ifndef KICKOS_KERNEL_STACKS
#define KICKOS_KERNEL_STACKS 0
#endif

/* The software block PendSV and the SVC fastpath push below the live PSP, counted off the
 * EMITTED code (arm-none-eabi-objdump on a linked image):
 *
 *   stmdb r0!, {r4-r11, lr}     9 words              36
 *   vstmdbeq r0!, {s16-s31}    16 words, when the FP frame is live, so BELOW the block
 *                              above and only on an __ARM_FP build            +64
 *
 * arch_armv7m.cc asserts both against the register counts they price, and FRAME_MAX against
 * their sum. FRAME_MAX is a plain integer and not an expression: the gate scrapes the
 * enforced figures out of this header as immediates. */
#define KICKOS_ARMV7M_TRAP_FRAME 36
#define KICKOS_ARMV7M_TRAP_FRAME_FP 64
#define KICKOS_ARMV7M_TRAP_FRAME_MAX 100

/* THE SVC CLASS, structural half: the bytes below the guarded PSP whose writers are the
 * hardware and one assembly prologue, so no call graph sees them. Write P0 for the PSP the
 * guard validates and P1 for the PSP the SVC's own exception return leaves, which is where
 * svc_trampoline starts.
 *
 *   P1 = P0 + 32   the hardware frame that exception return unstacks. 104 when the thread
 *                  carries an FP frame, and the pessimal pairing below wants the SMALLER
 *                  credit, so 32.
 *   -8             svc_trampoline's own prologue, and BOTH DESIGNS SPEND EXACTLY EIGHT
 *                  BYTES on it: `push {r0, r1}`, the two scratch registers the transfer
 *                  needs to reach g_arch_current, at KICKOS_KERNEL_STACKS 1, and
 *                  `push {r12, lr}`, a3 plus the caller-return, at 0. Those eight bytes sit
 *                  inside the frame the return just popped, so they cost nothing below P0
 *                  by themselves; they are here because what follows is measured from them.
 *   -104           the hardware frame a device IRQ or SysTick stacks if it preempts the
 *                  trampoline. PRIO_DEVICE (0x30) is above PRIO_SVCALL (0xE0), and the
 *                  trampoline runs in THREAD mode, so ARMv7-M stacks it on the PSP and
 *                  nothing checks it: this term is what makes the class a privilege
 *                  question and not only a sizing one.
 *   -100           the PendSV that tail-chains behind it and pushes the software block one
 *                  level lower. It cannot repeat: PendSV switches away and pops the block
 *                  on resume, and a further exception nests on the MSP, so at most ONE
 *                  exception is ever stacked on a thread's PSP.
 *
 * 32 - 8 - 104 - 100 is -180, so 180 below P0. The STKALIGN pad CANCELS and is not a term:
 * entry spends 4 exactly when the pre-exception SP was 4-mod-8, which is also when P1 - 8 is
 * 4-mod-8 and the preempting entry spends its own 4. arch_syscall_reg reaches that posture,
 * its `push {r3-r11, lr}` being ten words. At KICKOS_KERNEL_STACKS 0 the dispatch descends
 * BELOW all of this on the same PSP, and that half is measured separately as
 * KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVC.
 *
 * The 104/100 pair is the FP-live posture while the 32 credit is the FP-absent one, and that
 * pessimal pairing is reachable: a thread traps with no FP frame, then the ISR executes one
 * FP instruction, so CONTROL.FPCA is set by the time the tick lands. Whether the kernel emits
 * an FP instruction is a codegen decision and is NOT gated, so the 136 bytes are reserved
 * unconditionally. A soft-float board never sets FPCA and those bytes go unspent.
 *
 * FRAME_MAX is NOT added on top: the fastpath arm's push is an alternative to this window,
 * not something below it, and 180 dominates it. arch_armv7m.cc asserts the domination. */
#define KICKOS_ARMV7M_TRAP_NEST_SVC 180

/* THE SVCK CLASS, structural half: what stands between the top of a kernel block and the
 * first byte syscall_dispatch may use, plus what a preemption puts below the deepest byte it
 * uses. Write T for ctx.kernel_sp, the block's top.
 *
 *   -16    the continuation header svc_trampoline lays at T: a3 (the fifth argument, which
 *          the AAPCS passes on the stack), the caller-return it exits to, the PSP the
 *          thread resumes on, and the word that keeps SP 8-byte aligned for the call.
 *          syscall_dispatch runs below it.
 *   -4     the STKALIGN pad a preempting exception spends. It does NOT cancel here the way
 *          it does in the SVC window: what sits above is a chain of compiler frames, whose
 *          sub is a multiple of 4, so the deepest SP can be 4-mod-8 on its own.
 *   -104   the hardware frame a device IRQ or SysTick stacks when it preempts the dispatch.
 *          The dispatch runs in THREAD mode, so ARMv7-M stacks it on the PSP, which is this
 *          block.
 *   -100   the PendSV that tail-chains behind it, by the same single-nesting argument as
 *          the SVC class above.
 *
 * 16 + 4 + 104 + 100 is 224. The FP terms are reserved unconditionally for the same reason
 * the SVC class reserves them. */
#define KICKOS_ARMV7M_TRAP_NEST_SVCK 224

/* Worst-case bytes the kernel's C dispatch descends BELOW all of that, per class.
 *
 *   _PENDSV   0  handler mode uses SP_main, so everything the switcher calls descends
 *                there instead of on this stack.
 *   _SVC    656  THE UNCONVERTED DESIGN ONLY (KICKOS_KERNEL_STACKS 0): svc_trampoline runs
 *                syscall_dispatch on the caller's own PSP, so the whole tree descends
 *                there. Measured 648, rounded up to a 16-byte boundary. At
 *                KICKOS_KERNEL_STACKS 1 nothing of the dispatch reaches this stack, the
 *                trampoline having moved SP first, and the figure goes unspent: it is not
 *                zeroed because the same header serves both designs and the gate scrapes it
 *                for the SVC class on every preset, the call graph being unable to see
 *                which design linked.
 *   _SVCK   760  THE TRANSFER (KICKOS_KERNEL_STACKS 1): the same dispatch measured on the
 *                kernel block, with the panic tail COUNTED. Measured 760 on frdmk64f-st.
 *
 * THE MEASUREMENT is the deepest over the presets tests/static/trap_redzone_roots.txt
 * declares for this arch (MinSizeRel, -fcallgraph-info=su,da), taken as the longest weighted
 * path over the merged call graph from the one root svc_trampoline branches to. 648 on
 * frdmk64f-st and teensy41-st:
 *
 *   syscall_dispatch[24] -> syscall_body[112] -> thread_spawn[256] -> thread_create[80]
 *   -> task_for[16] -> domain_for[32] -> grant_region_admissible[24]
 *   -> grant_hits_reserved[88] -> arch_reserved_blocks[16]
 *
 * The four MPS2 presets measure 632 on the same chain without that chip-specific tail,
 * f411disco-st and blackpill-st 644, f302nucleo-st and bluepill-c8-st 444, and due-st 436.
 *
 * WHY _SVC IS STILL 656 WHEN ONLY f302nucleo-st AND due-st TAKE THAT PATH. The two halves of
 * a class are enforced fleet-wide, because the graph cannot say which design an object
 * linked: the gate measures the SVC class on every armv7m preset and 656 has to dominate all
 * of them. Sizing it to f302nucleo's own 444 would make the figure a claim about which
 * boards happen to be unconverted, which is exactly the sample defect this file's history is
 * a record of.
 *
 * THE PANIC TAIL IS COUNTED IN _SVCK AND EXCLUDED FROM _SVC, and the asymmetry is the whole
 * of the difference between 760 and 656. A kernel array has no spawn floor to clear, so
 * trap_redzone_roots.txt declares SVCK stack=kernel and a stack=kernel class is measured
 * with no exclusion at all; a thread stack does have one, and counting the tail there would
 * put the red zone above every board's KICKOS_MIN_STACK_SIZE. Counting it also makes the
 * SVCK figure CONSOLE-SHAPED, so it is the worse of this arch's REGISTERED presets and not
 * one board's reading: qemu 672, qemu-m3 672, frdmk64f-st 760, f302nucleo-st 680. Six more
 * presets are declared for a hand run, and two of them measure higher on the EXCLUDED chain
 * than any registered one did, which is the standing warning that a figure is only as wide
 * as its sample.
 *
 * On arm-none-eabi the compiler's reported frame is the prologue push plus the sub, with the
 * return address arriving in LR and no incoming slot, so a frameless leaf is 0 and a chain
 * is not missing a per-level 4. thread_spawn calibrates it: `stmdb sp!, {r4-r11, lr}` (36)
 * plus `sub sp, #220` is the 256 the graph reports.
 *
 * EXCLUDED FROM _SVC, and the residual it leaves: the noreturn kpanic tail, which every
 * KICKOS_ASSERT in the dispatch reaches, through either of its two doors. Including it takes
 * the deepest measurement to 760, which would put the red zone at 940 and above every
 * armv7m board's floor, so a thread spawned at the floor could not make a syscall at all.
 * The residual is that a kernel assertion firing while a thread is parked at the very bottom
 * of its red zone has the console writer descend below stack_lo, privileged, with the system
 * already terminating. check_trap_redzone.sh prints both figures.
 *
 * The winning chain runs through an INDIRECT call, the SchedPolicy hook table:
 * tests/static/trap_redzone_indirect.txt binds each such site to the one slot its source line
 * calls, and the gate refuses to answer at all while a reachable site is unbound. */
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_PENDSV 0
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVC 656
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVCK 760

/* What each guarded site enforces: room below the live PSP, in bytes. Both are loaded with
 * movw, whose imm16 range covers anything the floor can hold, so a figure that outgrew the
 * encoding fails to assemble rather than truncating. These are also the figures the red-zone
 * gate scrapes as each class's non-measured half plus its measured one.
 *
 * ONLY PENDSV TAKES A RUN-TIME FP TERM, and the SVC site must not. PendSV's push IS the
 * {s16-s31} block, so the term is the difference between what it stores with a live FP frame
 * and without. NEST_SVC is a structural constant that already bounds both entry postures,
 * which is what the 32 credit above means: walk the window from P0 for each one and the
 * FP-absent entry is the worse of the two, because the 72 extra bytes the hardware consumes
 * ABOVE P0 on an FP-live entry are 72 bytes svc_trampoline then starts higher by.
 *
 *   entry frame 32:   trampoline at P0+32,  window reaches P0-180
 *   entry frame 104:  trampoline at P0+104, window reaches P0-108
 *
 * So 180 covers both, exactly for the first and with 72 bytes spare for the second. The same
 * cancellation carries the unconverted design's dispatch, which starts from the trampoline's
 * SP either way. */
#define KICKOS_ARMV7M_TRAP_NEED_PENDSV \
    (KICKOS_ARMV7M_TRAP_FRAME + KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_PENDSV)

/* THE ONE FIGURE THE SVC SITE ENFORCES, resolved by which entry design this build compiles,
 * so switch.S's guard, arch_armv7m.cc's floor assert, kickos_armv7m_bad_psp's reported count
 * and user/apps/common/pspguard's expectation are one value and not four agreements. At
 * KICKOS_KERNEL_STACKS 1 the dispatch is on the kernel block and the site charges the window
 * alone; at 0 it charges the window plus the dispatch. */
#if KICKOS_KERNEL_STACKS
#define KICKOS_ARMV7M_TRAP_NEED_SVC KICKOS_ARMV7M_TRAP_NEST_SVC
#else
#define KICKOS_ARMV7M_TRAP_NEED_SVC \
    (KICKOS_ARMV7M_TRAP_NEST_SVC + KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVC)
#endif

/* What one kernel block has to hold, which is a requirement on KICKOS_KERNEL_STACK_SIZE and
 * not a bound anything refuses at run time: every byte of it is written by privileged code
 * through a pointer the kernel seated. arch_armv7m.cc static_asserts the block against it,
 * and check_trap_redzone.sh compares the same pair against the block the board configured. */
#define KICKOS_ARMV7M_TRAP_NEED_SVCK \
    (KICKOS_ARMV7M_TRAP_NEST_SVCK + KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVCK)

/* struct arch_context field offsets the trap sites read as plain displacements. switch.S
 * .equ's from these and arch_armv7m.cc static_asserts offsetof against them. UNCONDITIONAL,
 * because the telemetry-only trace_tid is the LAST field precisely so that no build posture
 * shifts them, which would make the guard compare a PSP against a trace id. kernel_sp sits
 * ahead of it for that reason and not after. */
#define KICKOS_ARMV7M_CTX_OFF_STACK_LO 12
#define KICKOS_ARMV7M_CTX_OFF_STACK_HI 16
#define KICKOS_ARMV7M_CTX_OFF_KERNEL_SP 20
#define KICKOS_ARMV7M_CTX_OFF_TRACE_TID 24

#endif /* KICKOS_ARCH_ARMV7M_TRAP_STACK_H */
