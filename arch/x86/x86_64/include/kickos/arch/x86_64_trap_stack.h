/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * What the x86_64 entries (trap_x86_64.S, switch.S) require of the stacks they build frames on.
 * Plain integer #defines only: the gate reads each one as an immediate.
 *
 * A RING 3 ENTRY BUILDS ITS FRAME ON THE THREAD'S KERNEL BLOCK: the interrupt gate through the
 * task-state segment's rsp0, the syscall entry through the per-core block. The gate clears the
 * interrupt flag and IA32_FMASK clears it for SYSCALL, so nothing nests on a block under IRQK or
 * SYSK. A RING 0 INTERRUPT RUNS ITS WHOLE DISPATCH ON THE STACK IT INTERRUPTED: idle's, a
 * privileged thread's own, or a death-path stub's block. The double fault, the NMI and the
 * machine check take interrupt-stack-table slots of their own (desc.h).
 *
 * A PRIVILEGED CALLER'S SYSCALL IS A CALL: arch_syscall jumps straight to the dispatch on the
 * caller's own stack with interrupts as they were.
 *
 * NO INTERRUPT NESTS BELOW A SWITCH FRAME: arch_switch runs under the kernel IrqLock and the
 * frame it saves carries that masked flag. So each descent with interrupts live is two classes,
 * the interrupt nested at its deepest byte with the switch cut, and the switch with nothing
 * below it.
 *
 * Each figure is the deepest reading over the x86_64 presets at its core count. Thread-stack figures are rounded
 * up to the next multiple of 64, kernel-block ones stay at the measurement, and the arrays
 * sized once per image take the next multiple of 64 strictly above. x86_64 gcc counts a
 * function's own return-address slot in its frame, so the assembly bodies below do too.
 */

#ifndef KICKOS_ARCH_X86_64_TRAP_STACK_H
#define KICKOS_ARCH_X86_64_TRAP_STACK_H

/* struct trap_frame, the frame every entry builds from a 16-byte-aligned top: five hardware
 * words, the stub's error code and vector, fifteen registers. arch_x86_64.cc asserts it. */
#define KICKOS_X86_64_TRAP_FRAME 176

/* The same frame delivered at ring 0 onto the stack it interrupts, which the processor first
 * aligns down to 16 bytes (Intel SDM Vol 3, 6.14.2): up to 8 more. */
#define KICKOS_X86_64_TRAP_FRAME_IRQ 184

/* The frame term where nothing interrupts: a class ending in the masked switch. */
#define KICKOS_X86_64_TRAP_FRAME_NONE 0

/* IRQ, IRQK and the frame term of every class an interrupt lands under. 352 on
 * qemu-x86_64, a timer expiry's wake re-arming the slice through pick_and_seat;
 * 320 on qemu-x86_64-bench. */
#define KICKOS_X86_64_TRAP_DEPTH_IRQ 352

/* A ring 0 interrupt's whole extent below the rsp it interrupts, FRAME_IRQ + DEPTH_IRQ, which
 * arch_x86_64.cc asserts. */
#define KICKOS_X86_64_TRAP_NEST 536

/* The ring 3 syscall on the block. 960 on qemu-x86_64-bench, the bench arm that prints:
 *   syscall_dispatch[96] -> bench_irq_sweep[128] -> dist_print_fmt[128] -> kprintf_paced[368]
 *   -> kconsole_write[8] -> console_emit[64] -> arch_console_write[8]
 *   -> console_tx_insert_line[64] -> console_write_line_sync[48] -> arch_console_write_sync[32]
 *   -> com1_putc[8] -> com1_slot_free[8]
 * 824 on qemu-x86_64, a reply-receive whose exit teardown wakes into the switch. */
#define KICKOS_X86_64_TRAP_DEPTH_SYSK 960

/* The same dispatch on a privileged caller's own stack with an interrupt nested below. 960 on
 * qemu-x86_64-bench down SYSK's chain, 744 on qemu-x86_64, a spawn's domain lookup. NEST + 960
 * = 1496, rounded up to 1536 for KICKOS_MIN_STACK_SIZE. */
#define KICKOS_X86_64_TRAP_DEPTH_SYSPRIV 960

/* The same dispatch through the switch: 960 on qemu-x86_64-bench, 824 on qemu-x86_64. */
#define KICKOS_X86_64_TRAP_DEPTH_SYSPRIVSW 960

/* The double-fault, NMI and machine-check slots: kickos_x86_64_trap on a static array, 320
 * down IRQ's chain, which bounds the reporter those vectors actually take. */
#define KICKOS_X86_64_TRAP_DEPTH_IST 384

/* The fault and slay stubs on the block with an interrupt nested below. 472 on
 * qemu-x86_64, the teardown's wake re-arming the slice; 456 on qemu-x86_64-bench.
 * NEST + 472 = 1008 against 4092 above the canary. */
#define KICKOS_X86_64_TRAP_DEPTH_EXITK 472

/* The same stubs through the switch. 608 on qemu-x86_64-bench:
 *   ... -> pick_and_seat[48] -> arch_switch[32] -> kickos_x86_64_switch_now[192]
 *   -> kickos_bench_switch_done[8] */
#define KICKOS_X86_64_TRAP_DEPTH_EXITKSW 608

/* kickos_thread_return on a privileged thread's own stack with an interrupt nested below: 456
 * on qemu-x86_64-bench, down EXITK's chain from kickos_thread_return[16]. */
#define KICKOS_X86_64_TRAP_DEPTH_RET 512

/* The same return through the switch: 608 on qemu-x86_64-bench, down EXITKSW's chain. */
#define KICKOS_X86_64_TRAP_DEPTH_RETSW 640

/* idle_entry's own frame and arch_idle_wait's, above the interrupt idle waits for: 24 on both
 * presets. */
#define KICKOS_X86_64_TRAP_DEPTH_IDLE 64

/* The panic reporter on its own array, below the null return word kickos_panic_stack_enter
 * pushes. 272 on qemu-x86_64-bench:
 *   kickos_panic_report[16] -> kputs[16] -> kconsole_write[8] -> console_emit[64] -> ...
 *   -> com1_putc[8] -> com1_slot_free[8] */
#define KICKOS_X86_64_PANIC_FRAME 8
#define KICKOS_X86_64_PANIC_DEPTH 320

/* The SMP doorbell, lock wait and route service lengthen reachable call chains. These bounds
 * cover qemu-x86_64-smp12's callgraph after every reachable indirect site was bound.
 * The largest thread zone is 760 + 1536 = 2296 bytes, rounded to a 2304-byte spawn floor. */
#if KICKOS_KERNEL_CORES > 1
#undef KICKOS_X86_64_TRAP_DEPTH_IRQ
#define KICKOS_X86_64_TRAP_DEPTH_IRQ 576
#undef KICKOS_X86_64_TRAP_NEST
#define KICKOS_X86_64_TRAP_NEST 760
#undef KICKOS_X86_64_TRAP_DEPTH_SYSK
#define KICKOS_X86_64_TRAP_DEPTH_SYSK 1536
#undef KICKOS_X86_64_TRAP_DEPTH_SYSPRIV
#define KICKOS_X86_64_TRAP_DEPTH_SYSPRIV 1536
#undef KICKOS_X86_64_TRAP_DEPTH_SYSPRIVSW
#define KICKOS_X86_64_TRAP_DEPTH_SYSPRIVSW 1536
#undef KICKOS_X86_64_TRAP_DEPTH_IST
#define KICKOS_X86_64_TRAP_DEPTH_IST 576
#undef KICKOS_X86_64_TRAP_DEPTH_EXITK
#define KICKOS_X86_64_TRAP_DEPTH_EXITK 896
#undef KICKOS_X86_64_TRAP_DEPTH_EXITKSW
#define KICKOS_X86_64_TRAP_DEPTH_EXITKSW 896
#undef KICKOS_X86_64_TRAP_DEPTH_RET
#define KICKOS_X86_64_TRAP_DEPTH_RET 896
#undef KICKOS_X86_64_TRAP_DEPTH_RETSW
#define KICKOS_X86_64_TRAP_DEPTH_RETSW 896
#undef KICKOS_X86_64_PANIC_DEPTH
#define KICKOS_X86_64_PANIC_DEPTH 512
#endif

#endif
