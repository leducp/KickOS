/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The extents the ARMv7-M trap sites (switch.S) reserve: one at the BOTTOM of a thread's
 * stack before they write through the live PSP, one at the top of that thread's per-thread
 * KERNEL block, which the syscall trap transfers to.
 *
 * AN EXTENT AND NOT A POINTER TEST. Exception entry stacks the HARDWARE frame with the
 * PRE-exception privilege, so the MPU already refuses a kernel-aimed PSP as MSTKERR. The
 * {r4-r11, EXC_RETURN} block pushed BELOW that frame is written in handler mode and refused
 * by nothing, and its last word is the EXC_RETURN the resume branches through: a neighbour
 * owning that word rewrites it to 0xFFFFFFF1 and resumes the victim privileged. So the bound
 * is on the room REMAINING below the PSP.
 *
 * KICKOS_KERNEL_STACKS picks between two syscall entry designs:
 *   1  THE TRANSFER. svc_trampoline relocates SP onto the caller's own kernel block
 *      (ctx.kernel_sp) before dispatching, so the dispatch is a requirement on
 *      KICKOS_KERNEL_STACK_SIZE. Every HAS_MPU armv7m chip.
 *   0  THE RED ZONE. svc_trampoline dispatches on the caller's PSP and the SVC site refuses
 *      a PSP without room for all of it. stm32f302 and sam3x8e, which have no MPU.
 *
 * Four classes, separated by WHICH STACK they land on:
 *   PENDSV  the switcher's push on the interrupted thread's stack. PendSV_Handler runs in
 *           HANDLER mode, where ARMv7-M forces SP_main, so its kernel-descent term is 0 by
 *           handler mode rather than by measurement.
 *   SVC     the syscall trap's writes on the interrupted thread's stack.
 *   SVCK    the syscall dispatch on the caller's kernel block, at KICKOS_KERNEL_STACKS 1.
 *   EXIT    the death path, below.
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

/* The software block PendSV and the SVC fastpath push below the live PSP, counted off the
 * emitted code:
 *   stmdb r0!, {r4-r11, lr}     9 words                                        36
 *   vstmdbeq r0!, {s16-s31}    16 words, only on an __ARM_FP build            +64
 *
 * FRAME_MAX is a plain integer and not an expression: the gate scrapes the enforced figures
 * out of this header as immediates. arch_armv7m.cc asserts each against the registers it
 * prices, and FRAME_MAX against their sum. */
#define KICKOS_ARMV7M_TRAP_FRAME 36
#define KICKOS_ARMV7M_TRAP_FRAME_FP 64
#define KICKOS_ARMV7M_TRAP_FRAME_MAX 100

/* THE SVC CLASS, structural half: the bytes below the guarded PSP written by the hardware and
 * one assembly prologue, which no call graph sees. P0 is the PSP the guard validates, P1 the
 * PSP the SVC's exception return leaves, where svc_trampoline starts.
 *
 *   P1 = P0 + 32   the hardware frame exception return unstacks. 104 with an FP frame, and
 *                  the pessimal pairing below wants the SMALLER credit, so 32.
 *   -8             svc_trampoline's prologue. Both designs spend exactly eight bytes:
 *                  `push {r0, r1}` at KICKOS_KERNEL_STACKS 1, `push {r12, lr}` at 0.
 *   -104           the hardware frame a device IRQ or SysTick stacks if it preempts the
 *                  trampoline. PRIO_DEVICE (0x30) is above PRIO_SVCALL (0xE0) and the
 *                  trampoline runs in THREAD mode, so ARMv7-M stacks it on the PSP.
 *   -100           the PendSV that tail-chains behind it. It cannot repeat: PendSV switches
 *                  away and pops the block on resume, and a further exception nests on the
 *                  MSP, so at most ONE exception is ever stacked on a thread's PSP.
 *
 * THE STKALIGN PAD IS NOT A TERM. Both SVC sites are reached by a bl from C, so SP is
 * 8-aligned there, and neither prologue moves it off. It would also cancel if one did: entry
 * spends 4 exactly when the pre-exception SP was 4-mod-8, and that same case puts P0 4 bytes
 * lower, so both parities reach P0 - 180. A future entry point pushing an odd number of words
 * needs no term added here.
 *
 * The 104/100 pair is the FP-live posture while the 32 credit is the FP-absent one, and that
 * pairing is reachable: a thread traps with no FP frame, then the ISR executes one FP
 * instruction, so CONTROL.FPCA is set by the time the tick lands. Whether the kernel emits an
 * FP instruction is a codegen decision and is NOT gated, so the 136 bytes are unconditional.
 *
 * FRAME_MAX is NOT added on top: the fastpath arm's push is an alternative to this window,
 * not something below it, and 180 dominates it. arch_armv7m.cc asserts the domination. */
#define KICKOS_ARMV7M_TRAP_NEST_SVC 180

/* The same window with the dispatch inside it, which is the KICKOS_KERNEL_STACKS 0 posture.
 * One term is added, the STKALIGN pad the preempting entry spends: the cancellation above
 * holds for a fixed window walked from an 8-aligned SP, and the dispatch's own frames are
 * word-granular, so the deepest SP can be 4-mod-8 on its own.
 *
 * It is a frame term and not rounding in the depth. Here the gate compares DEPTH >= D for
 * every parity of D; folded into the depth it would compare D against a figure that already
 * spent 4 on the pad, and pass a dispatch 4 bytes past what the site can hold. */
#define KICKOS_ARMV7M_TRAP_NEST_SVC_DISPATCH 184

/* THE SVCK CLASS, structural half: what stands between the top of a kernel block (T =
 * ctx.kernel_sp) and the first byte syscall_dispatch may use, plus what a preemption puts
 * below the deepest byte it uses.
 *
 *   -16    the continuation header svc_trampoline lays at T: a3 (the fifth argument, passed
 *          on the stack by AAPCS), the caller-return, the PSP the thread resumes on, and the
 *          word that keeps SP 8-aligned for the call.
 *   -4     the STKALIGN pad a preempting exception spends. It does NOT cancel here: above it
 *          is a chain of compiler frames whose sub is a multiple of 4, so the deepest SP can
 *          be 4-mod-8 on its own.
 *   -104   the hardware frame a device IRQ or SysTick stacks. The dispatch runs in THREAD
 *          mode, so ARMv7-M stacks it on the PSP, which is this block.
 *   -100   the PendSV that tail-chains behind it, by the single-nesting argument above.
 *
 * The FP terms are unconditional for the same reason the SVC class makes them so. */
#define KICKOS_ARMV7M_TRAP_NEST_SVCK 224

/* Worst-case bytes the kernel's C dispatch descends BELOW all of that, per class.
 *
 *   _PENDSV   0  handler mode uses SP_main.
 *   _SVC    448  KICKOS_KERNEL_STACKS 0 ONLY: the whole dispatch tree runs on the caller's
 *                PSP. Measured 444 on f302nucleo, the deepest of the four presets that
 *                enforce this class, rounded up to the next multiple of 64. At
 *                KICKOS_KERNEL_STACKS 1 it goes unspent, and is not zeroed because the gate
 *                scrapes it for the SVC class on every preset, the call graph being unable
 *                to see which design linked.
 *   _SVCK        the same dispatch on the kernel block, posture-dependent, below.
 *
 * trap_redzone_roots.txt marks this class kstacks=0, so the gate skips it on every preset
 * whose KICKOS_KERNEL_STACKS is 1. Without that marker the figure would have to dominate all
 * 34 presets, including a bench bracket and a telemetry tail that only a CONVERTED board
 * compiles. 448 is the next multiple of 64 above 444 and of 16 as well; the next 64-byte step
 * is 512, so the 4 bytes over the measurement are rounding and not slack to spend.
 *
 * THE PANIC TAIL IS EXCLUDED HERE AND COUNTED IN _SVCK, and that asymmetry is most of the
 * difference between the two. A thread-stack class has a spawn floor to clear and counting
 * the tail there puts the red zone above every board's KICKOS_MIN_STACK_SIZE; a kernel array
 * has no floor, so trap_redzone_roots.txt declares SVCK stack=kernel and it is measured with
 * no exclusion. The margin the exclusion buys is thin: counted, the floor assert asks
 * 184 + 680 + 104 = 968 against a 960 KICKOS_MIN_STACK_SIZE; excluded it asks 736.
 *
 * The residual is that a kernel assertion firing while a thread sits at the very bottom of
 * its red zone has the console writer descend below stack_lo, privileged, with the system
 * already terminating.
 *
 * The winning chain runs through an INDIRECT call, the SchedPolicy hook table:
 * tests/static/trap_redzone_indirect.txt binds each such site to the one slot its source line
 * calls, and the gate refuses to answer while a reachable site is unbound. */
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_PENDSV 0
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVC 448

/* KICKOS_TELEMETRY is the knob, because the trace emitters and the ring drain arch_shutdown
 * runs are compiled by nothing else and the dispatch reaches them through exit_current as
 * well as through the panic tail. 768 to 1240 is the largest single posture effect in this
 * fleet, and one fleet-wide figure would make every non-telemetry board reserve
 * KICKOS_THREAD_SLOTS blocks of a tail its image does not contain: bluepill-c8 has 3 slots
 * and 224 spare bytes per slot, so the telemetry figure fails to link it.
 *
 * 768 is xmc4800-relax-st and -bench, whose USIC console backend is the deepest in the
 * armv7m fleet, so it is a console reading and not a dispatch one. 1240 is qemu-telem,
 * through arch_shutdown's telemetry tail, and qemu/telem is the only telemetry variant of any
 * armv7m board: a second one is where that figure gets re-measured, not assumed.
 *
 * The panic tail is COUNTED here: a kernel array has no spawn floor to clear, and the block
 * overflows into the adjacent slot or into kernel .bss, which is the console path it is
 * printing through, so it garbles the very report that caused it.
 *
 * NO FALLBACK #define, on purpose. KICKOS_TELEMETRY is an add_compile_definitions knob and
 * reaches out-of-tree consumers through kickos_core's INTERFACE definitions, so with
 * -Wundef -Werror an image that lost it fails to build; a fallback would silently reserve the
 * smaller figure. */
#if KICKOS_TELEMETRY
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVCK 1240
#else
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVCK 768
#endif

/* THE EXIT CLASS, structural half: NEST_SVCK's window without the 16-byte continuation
 * header, because no trampoline lays one here. The backend rewrites the exception frame so
 * the return lands in the stub with sp at kickos_fault_stack_top, and the stub is entered by
 * an exception return rather than by a call.
 *
 *   -4     the STKALIGN pad, which does not cancel, as in the SVCK window.
 *   -104   the hardware frame a device IRQ or SysTick stacks. The stub runs in THREAD mode.
 *   -100   the PendSV that tail-chains behind it.
 *
 * It is a frame term because the stub is PREEMPTIBLE, which is why this is not rv32imac's
 * KICKOS_RV_TRAP_FRAME_EXIT 0: that figure answers where the descent begins, not what lands
 * below it, and exit_current reschedules, so a preemption is the ordinary case here. */
#define KICKOS_ARMV7M_TRAP_NEST_EXIT 208

/* The measured descent of the three stubs a dying thread runs PRIVILEGED on its own stack:
 * kickos_thread_fault_exit, kickos_thread_slay_exit and kickos_thread_return. The fault stub
 * is the deepest at every posture, reaching the console through kprintf_fault; under
 * telemetry all three converge on arch_shutdown's drain.
 *
 * The kstacks=0 fallback and nothing else, so it carries no posture ladder: the six presets
 * that enforce it have neither a telemetry nor a bench variant and all six measure 576.
 * Where a block IS seated the two relocating stubs are EXITK below and kickos_thread_return
 * is RET. */
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_EXIT 576

/* THE SAME TWO STUBS ON THE KERNEL BLOCK. kickos_fault_stack_top answers with ctx.kernel_sp,
 * so the fault redirect and the slay rebuild both land at the block TOP, discarding whatever
 * dispatch frames it held. That discard is what keeps the block requirement the MAX of SVCK
 * and this rather than their sum: nested under a live dispatch frame the two would add, and
 * 992 + 784 fits no block on any arch.
 *
 * Measured with nothing excluded, a stack=kernel class having no spawn floor to clear. It
 * never binds: 208 + 584 = 792 against 1004 usable off telemetry, 208 + 952 = 1160 against
 * 1468 on, where SVCK asks 992 and 1464. rxv3 is the arch with least room for that to change,
 * its EXITK needing 168 more bytes before it displaced SYSK. */
#if KICKOS_TELEMETRY
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_EXITK 952
#else
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_EXITK 584
#endif

/* kickos_thread_return ALONE: an ordinary privileged thread's entry returning, with no fault
 * and no redirect to relocate it, so it runs on the thread's own stack under BOTH designs.
 * Relocating it needs an arch trampoline of its own.
 *
 * 312 is xmc4800-relax-bench and 296 the other 32 non-telemetry presets. Under telemetry it
 * is 824, so this root and not the two that moved is what carries the KICKOS_MIN_STACK_SIZE
 * pressure on qemu-telem. */
#if KICKOS_TELEMETRY
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_RET 824
#else
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_RET 312
#endif

/* What each guarded site enforces: room below the live PSP, in bytes. Both are loaded with
 * movw, whose imm16 range covers anything the floor can hold, so a figure that outgrew the
 * encoding fails to assemble rather than truncating.
 *
 * ONLY PENDSV TAKES A RUN-TIME FP TERM, and the SVC site must not. PendSV's push IS the
 * {s16-s31} block. NEST_SVC is a structural constant that already bounds both entry postures,
 * because the 72 extra bytes the hardware consumes ABOVE P0 on an FP-live entry are 72 bytes
 * svc_trampoline then starts higher by:
 *   entry frame 32:   trampoline at P0+32,  window reaches P0-180
 *   entry frame 104:  trampoline at P0+104, window reaches P0-108 */
#define KICKOS_ARMV7M_TRAP_NEED_PENDSV \
    (KICKOS_ARMV7M_TRAP_FRAME + KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_PENDSV)

/* Resolved by which entry design this build compiles, so switch.S's guard, arch_armv7m.cc's
 * floor assert, kickos_armv7m_bad_psp's reported count and user/apps/common/pspguard's
 * expectation are one value and not four agreements. */
#if KICKOS_KERNEL_STACKS
#define KICKOS_ARMV7M_TRAP_NEED_SVC KICKOS_ARMV7M_TRAP_NEST_SVC
#else
#define KICKOS_ARMV7M_TRAP_NEED_SVC \
    (KICKOS_ARMV7M_TRAP_NEST_SVC_DISPATCH + KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVC)
#endif

/* What one kernel block has to hold: a requirement on KICKOS_KERNEL_STACK_SIZE, not a bound
 * anything refuses at run time, every byte of it being written by privileged code through a
 * pointer the kernel seated. It resolves per KICKOS_TELEMETRY so that the Kconfig ceiling,
 * arch_armv7m.cc's static_assert and check_trap_redzone.sh all price the same posture:
 * 224 + 768 is 992 off, 224 + 1240 is 1464 on, and Kconfig adds the canary word and rounds
 * to 16. */
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
#define KICKOS_ARMV7M_CTX_OFF_TRACE_TID 24

#endif /* KICKOS_ARCH_ARMV7M_TRAP_STACK_H */
