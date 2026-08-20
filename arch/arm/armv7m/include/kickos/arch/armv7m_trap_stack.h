/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The extent the two ARMv7-M software pushes (switch.S) reserve at the BOTTOM of a
 * thread's stack before they agree to write through the live PSP. Plain #defines so
 * switch.S and arch_armv7m.cc read ONE source of truth.
 *
 * WHY AN EXTENT AND NOT A POINTER TEST. Exception entry stacks the HARDWARE frame ABOVE
 * the PSP with the PRE-exception privilege, so the MPU refuses a kernel-aimed PSP as
 * MSTKERR before any handler runs. The {r4-r11, EXC_RETURN} block PendSV and the SVC trap
 * push BELOW that frame is written in handler mode and is refused by nothing, and the last
 * word of it is the EXC_RETURN the resume branches through: a neighbour that owns the word
 * under the stack and rewrites it to 0xFFFFFFF1 resumes the victim in handler mode,
 * privileged. So the bound is on the room REMAINING below the PSP, and it must cover the
 * push PLUS whatever the kernel then runs on that same stack.
 *
 * TWO CLASSES, because the two guarded sites differ in exactly that second half:
 *
 *   PENDSV  the switcher's push, and nothing else. PendSV_Handler runs in HANDLER mode,
 *           where ARMv7-M forces SP_main, so every bl in it (kickos_arch_mpu_commit, the
 *           telemetry and bench hooks) descends on the MSP and not on the thread's stack.
 *           Its kernel-descent term is therefore 0, and that is a claim about handler mode
 *           rather than a measurement. The same holds for the SVC fastpath arm, whose
 *           kickos_ipc_fastpath call is also handler-mode: what the MSP has to hold is a
 *           kernel-stack SIZING question, not a privilege one.
 *
 *   SVC     the slow path, the one place kernel C runs on a thread-chosen stack.
 *           SVC_Handler rewrites the stacked PC and exception-returns into svc_trampoline,
 *           which runs PRIVILEGED IN THREAD MODE on that same PSP and calls
 *           syscall_dispatch. The MPU is not consulted for a privileged access, so the
 *           whole descent lands wherever the thread aimed the PSP.
 *
 * tests/static/check_trap_redzone.sh re-measures the descent figure below and fails when the
 * worst case exceeds what it reserves. The measured half and the structural half stay
 * SEPARATE macros: folded into one, a growing dispatch could eat the structural allowance
 * and the gate would still report room to spare.
 *
 * NO ALIGNMENT LEG, unlike rv32imac and rxv3: ARMv7-M defines SP bits[1:0] as RAZ/WI, so
 * there is no word-misaligned PSP for a guard to refuse. user/apps/common/pspguard writes
 * and reads back an SP with both bits set and reports what the core kept.
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
 * arch_armv7m.cc asserts both against the register counts they price, and FRAME_MAX
 * against their sum. FRAME_MAX is a separate plain integer rather than an expression
 * because the gate scrapes the enforced figures out of this header as immediates. */
#define KICKOS_ARMV7M_TRAP_FRAME 36
#define KICKOS_ARMV7M_TRAP_FRAME_FP 64
#define KICKOS_ARMV7M_TRAP_FRAME_MAX 100

/* THE SVC CLASS, structural half: the bytes below the guarded PSP that no call graph can
 * see, because their writers are the hardware and one assembly prologue. Write P0 for the
 * PSP the guard validates.
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
 * The 104/100 pair is the FP-live posture while the -32 credit is the FP-absent one, and
 * that pessimal pairing is reachable: a thread traps with no FP frame, then the dispatch or
 * the ISR executes one FP instruction, so CONTROL.FPCA is set by the time the tick lands.
 * Whether the kernel emits an FP instruction is a codegen decision and is NOT gated, so the
 * 136 bytes are reserved unconditionally. On a soft-float board the pairing cannot arise and
 * those bytes go unspent.
 *
 * The 36-byte push is NOT added on top: it is an alternative to this descent, not something
 * below it, and 180 dominates it. arch_armv7m.cc asserts the domination. */
#define KICKOS_ARMV7M_TRAP_NEST_SVC 180

/* Worst-case bytes the kernel's C dispatch descends BELOW all of that, per class.
 *
 *   _PENDSV    0  handler mode uses SP_main, so nothing the switcher calls is on this
 *                 stack.
 *   _SVC     656  measured 648, rounded up to a 16-byte boundary.
 *
 * THE MEASUREMENT is the deepest over the presets tests/static/trap_redzone_roots.txt
 * declares for this arch (MinSizeRel, -fcallgraph-info=su,da), taken as the longest
 * weighted path over the merged call graph from the one root svc_trampoline branches to.
 * 648 on frdmk64f-st and teensy41-st:
 *
 *   syscall_dispatch[24] -> syscall_body[112] -> thread_spawn[256] -> thread_create[80]
 *   -> task_for[16] -> domain_for[32] -> grant_region_admissible[24]
 *   -> grant_hits_reserved[88] -> arch_reserved_blocks[16]
 *
 * The four MPS2 presets measure 632 on the same chain without that chip-specific tail,
 * f411disco-st and blackpill-st 644, and due-st 436.
 *
 * On arm-none-eabi the compiler's reported frame is the prologue push plus the sub, with no
 * incoming return-address slot (the return address arrives in LR), so a frameless leaf is 0
 * and the chain above is not missing a per-level 4. thread_spawn calibrates it:
 * `stmdb sp!, {r4-r11, lr}` (36) plus `sub sp, #220` is the 256 the graph reports.
 *
 * The winning chain runs through an INDIRECT call, the SchedPolicy hook table, which no
 * call graph resolves on its own: tests/static/trap_redzone_indirect.txt binds each such
 * site to the one slot its source line calls, and the gate refuses to answer at all while a
 * reachable site is unbound.
 *
 * EXCLUDED, and the residual it leaves: the noreturn kpanic tail, which every
 * KICKOS_ASSERT in the dispatch reaches. Including it takes the deepest measurement to 736
 * (xmc4800-relax-st), which would put the red zone at 980 and above every armv7m board's
 * floor, so a thread spawned at the floor could not make a syscall at all. The residual is
 * that if a kernel assertion fires while a thread is parked at the very bottom of its red
 * zone, the console writer descends up to 80 bytes below stack_lo, privileged, and the
 * system is already terminating. Same exclusion as rv32imac and rxv3;
 * check_trap_redzone.sh prints both figures. */
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_PENDSV 0
#define KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVC 656

/* The whole red zone MINUS the measured descent, per class, which is what the gate adds the
 * measurement back onto and compares against KICKOS_MIN_STACK_SIZE. Plain integers for the
 * same scrape reason as FRAME_MAX; arch_armv7m.cc asserts both against their parts. The FP
 * term is inside them because the floor has to hold the worst posture, while the guard adds
 * it at run time only when EXC_RETURN says the FP frame is live. */
#define KICKOS_ARMV7M_TRAP_ZONE_FIXED_PENDSV 100
#define KICKOS_ARMV7M_TRAP_ZONE_FIXED_SVC 244

/* What each guarded push enforces: room below the live PSP, in bytes, before the FP term
 * the macro in switch.S adds when EXC_RETURN says the FP frame is live. Both are loaded
 * with movw, whose imm16 range covers anything the floor can hold, so a figure that
 * outgrew the encoding would fail to assemble rather than truncate. */
#define KICKOS_ARMV7M_TRAP_NEED_PENDSV \
    (KICKOS_ARMV7M_TRAP_FRAME + KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_PENDSV)
#define KICKOS_ARMV7M_TRAP_NEED_SVC \
    (KICKOS_ARMV7M_TRAP_NEST_SVC + KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVC)

/* struct arch_context field offsets the guard reads as plain displacements. ONE definition
 * rather than a literal in switch.S and a mirror in arch_armv7m.cc: switch.S .equ's from
 * these, arch_armv7m.cc static_asserts offsetof against them. UNCONDITIONAL, because the
 * telemetry-only trace_tid is the LAST field precisely so that no build posture shifts
 * them, which would make the guard compare a PSP against a trace id. */
#define KICKOS_ARMV7M_CTX_OFF_STACK_LO 12
#define KICKOS_ARMV7M_CTX_OFF_STACK_HI 16

#endif /* KICKOS_ARCH_ARMV7M_TRAP_STACK_H */
