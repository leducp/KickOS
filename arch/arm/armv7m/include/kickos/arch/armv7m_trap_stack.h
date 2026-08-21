/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The extent the two ARMv7-M software pushes (switch.S) reserve at the BOTTOM of a thread's
 * stack before they agree to write through the live PSP.
 *
 * WHY AN EXTENT AND NOT A POINTER TEST. Exception entry stacks the HARDWARE frame ABOVE the
 * PSP with the PRE-exception privilege, so the MPU refuses a kernel-aimed PSP as MSTKERR
 * before any handler runs. The {r4-r11, EXC_RETURN} block PendSV and the SVC trap push BELOW
 * that frame is written in handler mode and is refused by nothing, and the last word of it is
 * the EXC_RETURN the resume branches through: a neighbour that owns the word under the stack
 * and rewrites it to 0xFFFFFFF1 resumes the victim in handler mode, privileged. So the bound
 * is on the room REMAINING below the PSP, and it covers the push PLUS whatever the kernel
 * then runs on that same stack.
 *
 * TWO CLASSES, because the two guarded sites differ in exactly that second half:
 *
 *   PENDSV  the switcher's push, and nothing else. PendSV_Handler runs in HANDLER mode,
 *           where ARMv7-M forces SP_main, so every bl in it (kickos_arch_mpu_commit, the
 *           telemetry and bench hooks) descends on the MSP. Its kernel-descent term is
 *           therefore 0 by handler mode rather than by measurement. The SVC fastpath arm is
 *           handler-mode too, so its kickos_ipc_fastpath call makes the MSP a kernel-stack
 *           SIZING question and not a privilege one.
 *
 *   SVC     the slow path, the one place kernel C runs on a thread-chosen stack.
 *           SVC_Handler rewrites the stacked PC and exception-returns into svc_trampoline,
 *           which runs PRIVILEGED IN THREAD MODE on that same PSP and calls
 *           syscall_dispatch. The MPU is not consulted for a privileged access, so the
 *           whole descent lands wherever the thread aimed the PSP.
 *
 * tests/static/check_trap_redzone.sh re-measures the descent figure below and fails when the
 * worst case exceeds what it reserves. The measured half and the structural half stay
 * SEPARATE macros: folded into one, a growing dispatch could eat the structural allowance and
 * the gate would still report room to spare.
 *
 * ARMv7-M defines SP bits[1:0] as RAZ/WI, so a word-alignment test on a PSP is always true
 * and the guard carries no alignment leg. user/apps/common/pspguard writes an SP with both
 * bits set, reads it back, and reports what the core kept.
 */

#ifndef KICKOS_ARCH_ARMV7M_TRAP_STACK_H
#define KICKOS_ARCH_ARMV7M_TRAP_STACK_H

/* The software block both guarded sites push below the live PSP, counted off the EMITTED
 * code (arm-none-eabi-objdump on a linked image):
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
 * guard validates.
 *
 *   +8    svc_trampoline's own `stmdb sp!, {ip, lr}`. syscall_dispatch runs below it.
 *   -32   the hardware frame the SVC's own exception return UNSTACKS before svc_trampoline
 *         runs, so the trampoline starts 32 bytes ABOVE P0. It is 104 when the thread
 *         carries an FP frame; 32 is the smaller credit and so the safe one.
 *   +104  the hardware frame a device IRQ or SysTick stacks when it preempts
 *         svc_trampoline. That exception is taken FROM THREAD MODE, so ARMv7-M stacks it
 *         on the PSP, at the bottom of the descent, and nothing checks it: this term is
 *         what makes the class a privilege question and not only a sizing one.
 *   +100  the PendSV that tail-chains behind it and pushes the software block one level
 *         lower. It cannot repeat: PendSV switches away and pops the block on resume, and
 *         a further exception nests on the MSP, so at most ONE exception is ever stacked
 *         on a thread's PSP.
 *
 * The 104/100 pair is the FP-live posture while the -32 credit is the FP-absent one, and that
 * pessimal pairing is reachable: a thread traps with no FP frame, then the dispatch or the ISR
 * executes one FP instruction, so CONTROL.FPCA is set by the time the tick lands. Whether the
 * kernel emits an FP instruction is a codegen decision and is NOT gated, so the 136 bytes are
 * reserved unconditionally. A soft-float board never sets FPCA and those bytes go unspent.
 *
 * The 36-byte push is NOT added on top: it is an alternative to this descent, not something
 * below it, and 180 dominates it. arch_armv7m.cc asserts the domination. */
#define KICKOS_ARMV7M_TRAP_NEST_SVC 180

/* Worst-case bytes the kernel's C dispatch descends BELOW all of that, per class.
 *
 *   _PENDSV    0  handler mode uses SP_main, so everything the switcher calls descends
 *                 there instead of on this stack.
 *   _SVC     656  measured 648, rounded up to a 16-byte boundary.
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
 * f411disco-st and blackpill-st 644, and due-st 436.
 *
 * On arm-none-eabi the compiler's reported frame is the prologue push plus the sub, with the
 * return address arriving in LR and no incoming slot, so a frameless leaf is 0 and the chain
 * above is not missing a per-level 4. thread_spawn calibrates it: `stmdb sp!, {r4-r11, lr}`
 * (36) plus `sub sp, #220` is the 256 the graph reports.
 *
 * The winning chain runs through an INDIRECT call, the SchedPolicy hook table:
 * tests/static/trap_redzone_indirect.txt binds each such site to the one slot its source line
 * calls, and the gate refuses to answer at all while a reachable site is unbound.
 *
 * EXCLUDED, and the residual it leaves: the noreturn kpanic tail, which every KICKOS_ASSERT
 * in the dispatch reaches. Including it takes the deepest measurement to 736
 * (xmc4800-relax-st), which would put the red zone at 980, above every armv7m board's floor,
 * so a thread spawned at the floor could not make a syscall at all. The residual is that a
 * kernel assertion firing while a thread is parked at the very bottom of its red zone has the
 * console writer descend up to 80 bytes below stack_lo, privileged, with the system already
 * terminating. check_trap_redzone.sh prints both figures. */
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_PENDSV 0
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVC 656

/* What each guarded site enforces: room below the live PSP, in bytes. Both are loaded with
 * movw, whose imm16 range covers anything the floor can hold, so a figure that outgrew the
 * encoding fails to assemble rather than truncating. These are also the figures the red-zone
 * gate scrapes as each class's non-measured half plus its measured one.
 *
 * ONLY PENDSV TAKES A RUN-TIME FP TERM, and the SVC site must not. Its push IS the {s16-s31}
 * block, so the term is the difference between what it stores with a live FP frame and
 * without. NEST_SVC is a structural constant that already bounds both entry postures, which
 * is what the -32 credit above means: walk the descent from P0 for each one and the FP-absent
 * entry is the worse of the two, because the 72 extra bytes the hardware consumes ABOVE P0 on
 * an FP-live entry are 72 bytes the trampoline then starts higher by.
 *
 *   entry frame 32:   trampoline at P0+32,  descend 8+656 to P0-632,  +104, +100 -> P0-836
 *   entry frame 104:  trampoline at P0+104, descend 8+656 to P0-560,  +104, +100 -> P0-764
 *
 * So 836 covers both, exactly for the first and with 72 bytes spare for the second. Adding
 * FRAME_FP on top of it charged the same pessimism twice and made the SVC site demand 900 of
 * an FP-active thread that has only 960-104 = 856 below its PSP, so the guard refused a legal
 * empty thread spawned at the floor on every syscall it made. */
#define KICKOS_ARMV7M_TRAP_NEED_PENDSV \
    (KICKOS_ARMV7M_TRAP_FRAME + KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_PENDSV)
#define KICKOS_ARMV7M_TRAP_NEED_SVC \
    (KICKOS_ARMV7M_TRAP_NEST_SVC + KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVC)

/* struct arch_context field offsets the guard reads as plain displacements. switch.S .equ's
 * from these and arch_armv7m.cc static_asserts offsetof against them. UNCONDITIONAL, because
 * the telemetry-only trace_tid is the LAST field precisely so that no build posture shifts
 * them, which would make the guard compare a PSP against a trace id. */
#define KICKOS_ARMV7M_CTX_OFF_STACK_LO 12
#define KICKOS_ARMV7M_CTX_OFF_STACK_HI 16

#endif /* KICKOS_ARCH_ARMV7M_TRAP_STACK_H */
