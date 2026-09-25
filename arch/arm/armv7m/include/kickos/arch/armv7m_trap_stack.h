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
 *                PSP. Measured 444 on all SIX presets that enforce this class
 *                (bluepill-c8, bluepill-c8-st, due, due-st, f302nucleo, f302nucleo-st),
 *                rounded up to the next multiple of 64, so the 4 bytes over it are rounding
 *                and not slack. It is not zeroed at KICKOS_KERNEL_STACKS 1: the gate scrapes
 *                it on every preset, and trap_redzone_roots.txt marks the class kstacks=0.
 *   _SVCK        the same dispatch on the kernel block, posture-dependent, below.
 *
 * THE PANIC REPORTER IS ON NEITHER THIS CLASS NOR _SVCK: kpanic leaves the stack it was called
 * on before it prints (kickos_panic_stack_enter, switch.S).
 *
 * tests/static/trap_redzone_indirect.txt binds every reachable indirect site to the one slot
 * that call reaches, and the gate refuses to answer while a reachable site is unbound. */
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_PENDSV 0
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVC 448

/* KICKOS_TELEMETRY is the knob: the trace emitters and arch_shutdown's ring drain are compiled
 * by nothing else, and one fleet-wide figure would make every non-telemetry board reserve
 * KICKOS_THREAD_SLOTS blocks of a tail its image does not contain. bluepill-c8 has 3 slots
 * and 224 spare bytes per slot, so the telemetry figure fails to link it.
 *
 * 768 enforced against 736 measured, tied by the two bench presets f411disco-bench and
 * xmc4800-relax-bench, whose bench arm prints; the 33 ordinary presets read 444 to 648, and the
 * two partition nodes pizero2350-amp2-n0 and -n1 read 668. 1240 enforced against 1000 measured
 * at qemu-telem through arch_shutdown's telemetry tail, the only telemetry variant of any
 * armv7m board. Both are enforced above their measurement, as headroom.
 *
 * A node's self-test drives the doorbell's service body from inside the dispatch (amp_probe
 * -> forge_reply_depth_recovery -> node_service); what wins there is an ordinary far reply
 * publication, endpoint_reply_recv -> endpoint_reply_locked -> inbound_reply -> send. Reached
 * from the doorbell interrupt instead, the same body runs in handler mode on SP_main and is
 * UNMEASURED, per the PENDSV reason in tests/static/trap_redzone_roots.txt.
 *
 * NO FALLBACK #define: with -Wundef -Werror an image that lost KICKOS_TELEMETRY fails to build,
 * where a fallback would silently reserve the smaller figure. */
#if KICKOS_TELEMETRY
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVCK 1240
#else
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVCK 768
#endif

/* THE EXIT CLASS, structural half: NEST_SVCK's window without the 16-byte continuation
 * header, which no trampoline lays here. The backend rewrites the exception frame so the
 * return lands in the stub with sp at kickos_fault_stack_top.
 *
 *   -4     the STKALIGN pad, which does not cancel, as in the SVCK window.
 *   -104   the hardware frame a device IRQ or SysTick stacks. The stub runs in THREAD mode.
 *   -100   the PendSV that tail-chains behind it.
 *
 * Unlike rv32imac's KICKOS_RV_TRAP_FRAME_EXIT 0, the stub is PREEMPTIBLE: exit_current
 * reschedules. */
#define KICKOS_ARMV7M_TRAP_NEST_EXIT 208

/* The measured descent of the three stubs a dying thread runs PRIVILEGED on its own stack:
 * kickos_thread_fault_exit, kickos_thread_slay_exit and kickos_thread_return. The fault stub
 * is the deepest at every posture, reaching the console through kprintf_fault, which formats
 * into KDIAG_FAULT_LINE_MAX bytes and not the 256 an ordinary kprintf gets.
 *
 * The kstacks=0 fallback and nothing else, so it carries no posture ladder: the six presets
 * that enforce it have neither a telemetry nor a bench variant and all six measure 296.
 * Where a block IS seated the two relocating stubs are EXITK below and kickos_thread_return
 * is RET.
 *
 * 448 is enforced over the 296 measured, and 208 + 448 = 656 is the largest thread-stack
 * requirement on those six presets, above _SVC's 632 and 304 under the 960 floor. */
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_EXIT 448

/* THE SAME TWO STUBS ON THE KERNEL BLOCK. kickos_fault_stack_top answers with ctx.kernel_sp,
 * so the fault redirect and the slay rebuild both land at the block TOP, discarding whatever
 * dispatch frames it held. That discard keeps the block requirement the MAX of SVCK and this
 * rather than their sum.
 *
 * Rounded like a thread-stack figure because it never binds: SVCK wins the block on every
 * registered preset. Off telemetry it measures 296, and 320 on the two bench presets, under an
 * enforced 448; on telemetry 952 stands over 792, down arch_shutdown's drain.
 *
 * 208 + 448 = 656 against 1004 usable off telemetry, 208 + 952 = 1160 against 1468 on, where
 * SVCK asks 992 and 1464. */
#if KICKOS_TELEMETRY
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_EXITK 952
#else
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_EXITK 448
#endif

/* kickos_thread_return ALONE: an ordinary privileged thread's entry returning, with no fault
 * and no redirect to relocate it, so it runs on the thread's own stack under BOTH designs.
 *
 * 312 on both armv7m bench presets, f411disco-bench and xmc4800-relax-bench, under an enforced
 * 352: the IrqLock bracket samples the outermost masked window inline, which costs a stack slot
 * on the chain. 264 on the other 35 non-telemetry presets, under an enforced 312, so no
 * non-bench floor reserves for that bracket. Under telemetry it measures 784 under an enforced
 * 800, down arch_shutdown's drain, and carries the KICKOS_MIN_STACK_SIZE pressure on
 * qemu-telem. */
#if KICKOS_TELEMETRY
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_RET 800
#elif KICKOS_BENCH
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_RET 352
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
 * floor assert, kickos_arm_bad_psp's reported count and user/apps/common/pspguard's
 * expectation are one value and not four agreements. */
#if KICKOS_KERNEL_STACKS
#define KICKOS_ARMV7M_TRAP_NEED_SVC KICKOS_ARMV7M_TRAP_NEST_SVC
#else
#define KICKOS_ARMV7M_TRAP_NEED_SVC \
    (KICKOS_ARMV7M_TRAP_NEST_SVC_DISPATCH + KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVC)
#endif

/* What one kernel block has to hold: a requirement on KICKOS_KERNEL_STACK_SIZE, not a bound
 * anything refuses at run time, every byte of it being written by privileged code through a
 * pointer the kernel seated. It resolves per POSTURE, of which there are two, so that the
 * Kconfig ceiling, arch_armv7m.cc's static_assert and check_trap_redzone.sh all price the same
 * one: 224 + 768 is 992 with telemetry off and 224 + 1240 is 1464 on. Kconfig adds the canary
 * word and rounds to 16. */
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

/* THE PANIC REPORTER'S OWN STACK, which kickos_panic_stack_enter (switch.S) moves to before a
 * banner is printed. The PANIC console is priced here and on no other class of this arch: SVC,
 * SVCK, EXIT and EXITK reach the reporter through a body the callgraph walk stops at. The
 * fault reporter is a different chain, on the dying thread's own stack, and EXIT and EXITK
 * measure it.
 *
 * FRAME IS THE HARDWARE FRAME, KICKOS_ARMV7M_TRAP_FRAME_MAX, and not 0: PRIMASK does not mask
 * a HardFault, and a wild access inside the reporter stacks that frame on this array. A plain
 * integer because check_trap_redzone.sh scrapes it as an immediate; arch_armv7m.cc asserts the
 * two agree.
 *
 * Telemetry is its own size class, as it is for the kernel block: arch_shutdown's ring drain
 * sits under kfault_terminate, inside the reporter. 168 is the deepest non-telemetry reading,
 * at xmc4800-relax-bench, and 760 the telemetry one at qemu-telem, under an enforced 320 and
 * 832. */
#define KICKOS_ARMV7M_PANIC_FRAME 100
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
#define KICKOS_ARMV7M_PANIC_DEPTH 832
#else
#define KICKOS_ARMV7M_PANIC_DEPTH 320
#endif

#endif /* KICKOS_ARCH_ARMV7M_TRAP_STACK_H */
