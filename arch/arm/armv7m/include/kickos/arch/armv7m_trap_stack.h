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
 * 32 - 8 - 104 - 100 is -180, so 180 below P0. The STKALIGN pad IS NOT A TERM, for two
 * independent reasons, and the second is what makes it safe to omit:
 *
 *   NEITHER SVC SITE IS ENTERED 4-MOD-8. Both are reached by a bl from C, so AAPCS has SP
 *   8-byte aligned at that boundary, and neither prologue moves it off: arch_syscall pushes
 *   nothing at all (`ldr r12, [sp]` then `svc`), and arch_syscall_reg's `push {r3-r11, lr}`
 *   is TEN registers, 40 bytes, which is 0 mod 8. So entry spends no pad today.
 *
 *   AND IT WOULD CANCEL IF ONE DID. Walk the window from the true resume SP rather than
 *   from the modelled P1: entry spends 4 exactly when the pre-exception SP S was 4-mod-8,
 *   and that same case puts P0 4 bytes LOWER, so the two moves are equal and opposite. Both
 *   parities reach P0 - 180 exactly:
 *     S 0-mod-8: P0 = S - 32.  S - 8 is 0-mod-8, no pad, so S - 8 - 104 - 100 = P0 - 180.
 *     S 4-mod-8: P0 = S - 36.  S - 8 is 4-mod-8, pad 4, so S - 12 - 104 - 100 = P0 - 180.
 *   So a future entry point that pushes an odd number of words needs no term added here.
 *
 * At KICKOS_KERNEL_STACKS 0 the dispatch descends BELOW all of this on the same PSP, and
 * that half is measured separately as KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVC. The pad a
 * preemption spends against THAT depth does NOT cancel and is provisioned as its own term,
 * KICKOS_ARMV7M_TRAP_NEST_SVC_DISPATCH below.
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

/* THE SAME WINDOW WITH THE DISPATCH INSIDE IT, which is the KICKOS_KERNEL_STACKS 0 posture and
 * the only one the SVC class is enforced under. One term is added and it is the STKALIGN pad:
 *
 *   +4     the pad the PREEMPTING entry spends, which no longer cancels once compiler frames
 *          stand between the trampoline and the preemption point. The cancellation above
 *          holds because the window is a fixed 8 + 104 + 100 walked from an 8-aligned SP; the
 *          dispatch's own frames are word-granular, so the deepest SP can be 4-mod-8 on its
 *          own and the preempting entry then spends 4 that the entry-side pad does not hand
 *          back. NEST_SVCK carries exactly this term for exactly this reason, the block top
 *          being 8-aligned with a compiler chain below it, and the two are the same physics.
 *
 * WHY IT IS A FRAME TERM AND NOT ROUNDING IN THE DEPTH. Provisioned here, the requirement
 * becomes 184 + DEPTH >= 180 + D + pad, which reduces to DEPTH >= D for every parity of D,
 * and DEPTH >= D is exactly what check_trap_redzone.sh compares. Folded into the depth
 * instead, the gate would compare D against a figure that already spent 4 on the pad and
 * would pass a dispatch 4 bytes past what the site can hold. It was invisible while the
 * depth carried 208 bytes of unclaimed slack; it is not invisible at 448, where f302nucleo's
 * own D + pad is 448 exactly. */
#define KICKOS_ARMV7M_TRAP_NEST_SVC_DISPATCH 184

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
 *   _SVC    448  THE UNCONVERTED DESIGN ONLY (KICKOS_KERNEL_STACKS 0): svc_trampoline runs
 *                syscall_dispatch on the caller's own PSP, so the whole tree descends
 *                there. Measured 444 on f302nucleo, the deepest of the four presets that
 *                enforce this class, rounded up to the next multiple of 64. At
 *                KICKOS_KERNEL_STACKS 1 nothing of the dispatch reaches this stack, the
 *                trampoline having moved SP first, and the figure goes unspent: it is not
 *                zeroed because the same header serves both designs and the gate scrapes it
 *                for the SVC class on every preset, the call graph being unable to see
 *                which design linked.
 *   _SVCK        THE TRANSFER (KICKOS_KERNEL_STACKS 1): the same dispatch measured on the
 *                kernel block, with the panic tail COUNTED. Posture-dependent, the ladder
 *                below deriving both figures.
 *
 * THE MEASUREMENT is taken on every preset tests/static/trap_redzone_roots.txt declares for
 * this arch, which is now ALL THIRTY-FOUR (MinSizeRel, -fcallgraph-info=su,da), as the
 * longest weighted path over the merged call graph from the one root svc_trampoline branches
 * to. WHICH of those readings a figure has to cover is per class, and for _SVC it is the four
 * kstacks=0 presets alone. The deepest reading anywhere is 648 (frdmk64f-st, teensy41-st,
 * pizero2350-st) and it does NOT size this figure, those three being converted boards where
 * the same chain lands on the kernel block and is priced as SVCK:
 *
 *   syscall_dispatch[24] -> syscall_body[112] -> thread_spawn[256] -> thread_create[80]
 *   -> task_for[16] -> domain_for[32] -> grant_region_admissible[24]
 *   -> grant_hits_reserved[88] -> arch_reserved_blocks[16]
 *
 * WHY THE FIGURE IS f302nucleo's AND NOT THE FLEET'S, and why that is a DECLARED restriction
 * rather than an accident of which boards happen to be unconverted. The class belongs to the
 * unconverted design, so trap_redzone_roots.txt marks it kstacks=0 and the gate skips it,
 * printing the skip, on every preset whose KICKOS_KERNEL_STACKS is 1. Before that marker
 * existed the figure had to dominate all 34, and two of them measure over even the old 656
 * for postures those two boards never compile: xmc4800-relax-bench 760 and qemu-telem 920,
 * neither board having an unconverted variant at all. A fleet-wide figure would have had to
 * charge f302nucleo and due for a bench bracket and a telemetry tail their images do not
 * contain, which is why the answer was a marker and not a bigger number.
 *
 * SO IT IS 448 AND NOT 656, and the 208 bytes go to the two boards with the tightest stacks
 * in the fleet. The four presets that enforce it measure 444 (f302nucleo, f302nucleo-st) and
 * 436 (due, due-st). Three things make the tightening right rather than merely possible:
 *
 *   THE CLASS IS NOW BOUNDED BY DECLARATION. kstacks=0 limits it to those four presets, so
 *   the figure no longer has to cover a bench bracket or a telemetry tail that only a
 *   CONVERTED board compiles. That was the whole argument for 656 and the marker retires it.
 *
 *   AND EVERY DECLARED PRESET IS GATED. All 34 are in the ctest ladder, so a dispatch that
 *   grows past this figure FAILS a gate instead of quietly eating historical slack. Hidden
 *   slack is not a substitute for measurement; it is a measurement nobody will take.
 *
 *   IT IS NOT KNIFE-EDGE. The floor assert below asks for 184 + 448 + 104 = 736 against a
 *   960 KICKOS_MIN_STACK_SIZE, so 224 bytes stand under the floor.
 *
 * 448 is 444 rounded up to the next multiple of 64, the convention rv_trap_stack.h states for
 * a thread-stack figure, and it is the next multiple of 16 as well, so the two conventions
 * this fleet uses agree here rather than having to be chosen between. The 4 bytes over the
 * measurement are that rounding and NOT slack to spend: the next 64-byte step is 512.
 *
 * THE PANIC TAIL IS COUNTED IN _SVCK AND EXCLUDED FROM _SVC, and the asymmetry is most of
 * the difference between the two. A kernel array has no spawn floor to clear, so
 * trap_redzone_roots.txt declares SVCK stack=kernel and a stack=kernel class is measured
 * with no exclusion at all; a thread stack does have one, and counting the tail there would
 * put the red zone above every board's KICKOS_MIN_STACK_SIZE. Counting it also makes the
 * SVCK figure CONSOLE-SHAPED, so it is the worse of the whole declared set and not one
 * board's reading.
 *
 * On arm-none-eabi the compiler's reported frame is the prologue push plus the sub, with the
 * return address arriving in LR and no incoming slot, so a frameless leaf is 0 and a chain
 * is not missing a per-level 4. thread_spawn calibrates it: `stmdb sp!, {r4-r11, lr}` (36)
 * plus `sub sp, #220` is the 256 the graph reports.
 *
 * EXCLUDED FROM _SVC: the noreturn kpanic tail, which every KICKOS_ASSERT in the dispatch
 * reaches, through either of its two doors. A SEPARATE DECISION FROM THE FIGURE ABOVE, and it
 * would stand at any value of it: what the exclusion answers is which chains the measurement
 * walks, not how much room the walked chains get.
 *
 * WHAT MAKES IT LOAD-BEARING IS THE FLOOR ASSERT BELOW, not the red zone measured against the
 * floor directly. Counting the tail takes the deepest of the four presets that enforce this
 * class to 680 (f302nucleo and f302nucleo-st; due and due-st reach 656), and f302nucleo
 * carries -mfpu=fpv4-sp-d16 -mfloat-abi=softfp, so it takes the 104 FP-live entry leg.
 *
 * TWO FRAME TERMS PRICE THAT WINDOW AND THE MARGIN DEPENDS ON WHICH, so every figure below
 * names the one it uses. NEST_SVC 180 is the PHYSICAL window; NEST_SVC_DISPATCH 184 is the
 * GATE MODEL, which is 180 plus the STKALIGN pad the kstacks=0 posture stops cancelling.
 * Against the 960 KICKOS_MIN_STACK_SIZE, and 704 being 680 under this header's rounding
 * convention:
 *
 *   tail counted, physical     180 + 680 + 104 = 964, FOUR over
 *   tail counted, gate model   184 + 680 + 104 = 968, EIGHT over
 *   tail counted, rounded      180 + 704 + 104 = 988, TWENTY-EIGHT over
 *   tail counted, both         184 + 704 + 104 = 992, THIRTY-TWO over
 *   tail excluded, physical    180 + 448 + 104 = 732, 228 to spare
 *   tail excluded, gate model  184 + 448 + 104 = 736, 224 to spare
 *
 * So the exclusion is the whole distance between comfortable and unbuildable, and the
 * tightest of the six reads FOUR BYTES.
 *
 * THAT FOUR IS NOT THE OTHER FOUR, and they are kept apart on purpose: this one is how far
 * the counted tail overruns the floor, and DEPTH_SVC's is how far 448 stands over the 444 it
 * rounds. Conflating them is the failure mode, one being a margin and the other a rounding.
 *
 * The residual is that a kernel assertion firing while a thread is parked at the very bottom
 * of its red zone has the console writer descend below stack_lo, privileged, with the system
 * already terminating. check_trap_redzone.sh prints both figures.
 *
 * The winning chain runs through an INDIRECT call, the SchedPolicy hook table:
 * tests/static/trap_redzone_indirect.txt binds each such site to the one slot its source line
 * calls, and the gate refuses to answer at all while a reachable site is unbound. */
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_PENDSV 0
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVC 448

/* THE SVCK DEPTH IS POSTURE-DEPENDENT, and KICKOS_TELEMETRY is the knob, because the trace
 * emitters and the ring drain arch_shutdown runs are compiled by nothing else and the
 * dispatch reaches them through exit_current as well as through the panic tail. It is the
 * largest single posture effect in this fleet, 768 to 1240, and folding the two into one
 * fleet-wide number would make every non-telemetry board reserve KICKOS_THREAD_SLOTS blocks
 * of a tail its image does not contain. bluepill-c8 is the board that proves the point: 3
 * slots, and its arena has room for 224 more bytes per slot, so the telemetry figure applied
 * fleet-wide fails to link a board with no telemetry variant at all.
 *
 *   KICKOS_TELEMETRY 0, 768 on xmc4800-relax-st and xmc4800-relax-bench, which are the two
 *   deepest of the 30 presets at this posture. The tail is what makes the figure per board:
 *   the XMC4800 console backend is the deepest in the armv7m fleet, so it is a USIC reading
 *   and not a dispatch one. frdmk64f-st, teensy41-st, f411disco-st, blackpill-st and
 *   pizero2350-st measure 760, xmc4800-relax and f302nucleo 680, and the four MPS2 base
 *   presets 672.
 *
 *   KICKOS_TELEMETRY 1, 1240 on qemu-telem, through arch_shutdown's telemetry tail:
 *   chip_mps2.cc calls kickos_trace_report_counters and sched.h ends the system there.
 *
 * ONE BOARD'S READING, AND IT SAYS SO the way the rxv3 figure does. qemu/telem is the only
 * telemetry variant of any armv7m board, so 1240 is not the worse of a sample. A second
 * telemetry board is where this figure gets re-measured, not assumed.
 *
 * THAT TAIL IS NOT EXCLUDED, and the decision is deliberate rather than pending. The panic
 * tail is excluded from a THREAD-stack class because such a class has a spawn floor to
 * clear, and trap_redzone_roots.txt states that a kernel array has no floor, so the reasoning
 * does not carry. The remaining argument would be that an overflow during termination is
 * unobservable, and it is not: the block overflows into the adjacent slot or into kernel
 * .bss, and what it overflows INTO is the console path it is printing through, so it garbles
 * the very report that caused it.
 *
 * NO FALLBACK #define, on purpose. KICKOS_TELEMETRY is an add_compile_definitions knob,
 * always 0 or 1, and it reaches an out-of-tree consumer through kickos_core's INTERFACE
 * definitions as well. With -Wundef -Werror an image that lost it fails to build, where a
 * fallback would silently reserve the smaller of the two figures. */
#if KICKOS_TELEMETRY
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVCK 1240
#else
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVCK 768
#endif

/* THE EXIT CLASS, structural half: what a preemption puts below the deepest byte the DEATH
 * PATH uses. It is NEST_SVCK's window without the 16-byte continuation header, because no
 * trampoline lays one here: the backend rewrites the exception frame so the return lands in
 * the stub with sp at kickos_fault_stack_top, and the stub is entered by an exception return
 * rather than by a call.
 *
 *   -4     the STKALIGN pad a preempting exception spends. It does not cancel, for the same
 *          reason it does not in the SVCK window: what stands above is a chain of compiler
 *          frames whose sub is a multiple of 4, so the deepest sp can be 4-mod-8 on its own.
 *   -104   the hardware frame a device IRQ or SysTick stacks. The stub runs in THREAD mode.
 *   -100   the PendSV that tail-chains behind it.
 *
 * 4 + 104 + 100 is 208.
 *
 * IT IS A FRAME TERM BECAUSE THE STUB IS PREEMPTIBLE, and that is the whole reason this is
 * not rv32imac's KICKOS_RV_TRAP_FRAME_EXIT 0. That figure is justified there by the descent
 * STARTING at the stack top, which answers where it begins and not what lands below it;
 * exit_current reschedules, so a preemption is not merely possible here, it is the ordinary
 * case. */
#define KICKOS_ARMV7M_TRAP_NEST_EXIT 208

/* The measured descent of the three stubs a dying thread runs PRIVILEGED on its own stack:
 * kickos_thread_fault_exit, kickos_thread_slay_exit and kickos_thread_return. The fault stub
 * is the deepest of the three at every posture, reaching the console through kprintf_fault;
 * under telemetry all three converge on arch_shutdown's drain instead.
 *
 * THIS FIGURE IS NOW THE kstacks=0 FALLBACK AND NOTHING ELSE, so it carries no posture ladder:
 * the four presets that enforce it, f302nucleo, f302nucleo-st, due and due-st, have neither a
 * telemetry nor a bench variant, and all four measure 568. Where a block IS seated the two
 * relocating stubs are measured as EXITK below and kickos_thread_return as RET, this class
 * being skipped entirely by the stack=kernel and kstacks= markers. Folding the two would charge every non-telemetry board for
 * a tail its image does not contain.
 *
 * kickos_thread_return IS MEASURED SEPARATELY, as RET below, and cannot move: see there. */
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_EXIT 568

/* THE SAME TWO STUBS ON THE KERNEL BLOCK, which is where they run wherever one is seated.
 * kickos_fault_stack_top answers with ctx.kernel_sp, so the fault redirect and the slay
 * rebuild both land at the block TOP, discarding whatever dispatch frames it held. That
 * discard is what keeps the block requirement the MAX of SVCK and this rather than their sum:
 * nested under a live dispatch frame the two would add, and 992 + 784 fits no block on any
 * arch.
 *
 * MEASURED WITH NOTHING EXCLUDED, because a stack=kernel class has no spawn floor to clear.
 * That costs 120 bytes on qemu-telem alone, 824 excluded against 944, the panic tail running
 * kpanic -> kfault_terminate -> arch_shutdown; on the other 33 armv7m presets the excluded and
 * unexcluded readings are identical, so counting the tail costs nothing there.
 *
 * IT NEVER BINDS. 208 + 576 = 784 against 1004 usable off telemetry, and 208 + 944 = 1152
 * against 1468 on, where SVCK asks 992 and 1464. So the block requirement does not move and
 * no board pays a byte. rxv3 is the arch with least room for that to change: its EXITK would
 * have to grow 180 bytes before it displaced SYSK. */
#if KICKOS_TELEMETRY
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_EXITK 944
#else
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_EXITK 576
#endif

/* kickos_thread_return ALONE, which is the residual the move deliberately leaves. It is an
 * ordinary privileged thread's entry returning: no fault, no redirect, nothing to relocate it,
 * so it runs at whatever depth the entry returned from on the thread's own stack under BOTH
 * entry designs. Moving it would need an arch trampoline of its own and that is not this PR.
 *
 * 312 is xmc4800-relax-bench and 296 the other 32 non-telemetry presets. Under telemetry it is
 * 824, IDENTICAL to what the full class measured before the move, which is why PR 7 does not
 * relieve the KICKOS_MIN_STACK_SIZE pressure on qemu-telem at all: that floor leg is carried by
 * this root, not by the two that moved. */
#if KICKOS_TELEMETRY
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_RET 824
#else
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_RET 312
#endif

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
    (KICKOS_ARMV7M_TRAP_NEST_SVC_DISPATCH + KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVC)
#endif

/* What one kernel block has to hold, which is a requirement on KICKOS_KERNEL_STACK_SIZE and
 * not a bound anything refuses at run time: every byte of it is written by privileged code
 * through a pointer the kernel seated. arch_armv7m.cc static_asserts the block against it,
 * and check_trap_redzone.sh compares the same pair against the block the board configured.
 * It resolves per KICKOS_TELEMETRY through the depth above, so the Kconfig ceiling, the C
 * assert and the gate all price the same posture's figure: 224 + 768 is 992 at telemetry off,
 * 224 + 1240 is 1464 at rtt, and Kconfig adds the canary word and rounds to 16. */
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
