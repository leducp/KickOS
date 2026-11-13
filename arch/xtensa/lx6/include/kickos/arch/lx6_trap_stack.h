/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The extent _kickos_int_level1 (arch/xtensa/chip/esp32/startup.S) reserves at the bottom of
 * the interrupted thread's stack.
 *
 * LX6 has no MPU and no per-thread kernel block, so every level-1 interrupt runs its dispatch
 * on the stack it interrupted, and a KickOS syscall here is a plain call: the interrupt is the
 * only involuntary descent.
 *
 * FLUSH_WINDOWS adds nothing: it flushes each ancestor frame to that frame's own base save
 * area, which sits above the interruptee's sp.
 */

#ifndef KICKOS_ARCH_LX6_TRAP_STACK_H
#define KICKOS_ARCH_LX6_TRAP_STACK_H

#include <kickos/arch/core_stack.ld.h>

/* The interrupt frame: the a-registers, the 16 f-registers with FCR/FSR, PC and PS. Must equal
 * F_SIZE in xtensa_frame.h, which the entry subtracts; arch_xtensa.cc asserts the pair. */
#define KICKOS_LX6_TRAP_FRAME 256

/* What kickos_lx6_dispatch_l1 descends below that frame: 608 on esp32-wroom-smp, the console's
 * TX drain waking a writer waiting for ring room, whose switch re-acquires the kernel lock and
 * the lock's spin polls the doorbell (resched_after_wake -> pick_and_seat -> klock_attach
 * -> arch_kernel_lock -> kickos_doorbell_poll -> ...); 480 on esp32-wroom, a timeout's park
 * abort settling inheritance.
 *
 * The same figure bounds the unhandled-exception reporter on the stack that faulted: 608 at one
 * core, kprintf[336] into the console. Above one core its line waits on the kernel lock and serves
 * the doorbell there, 784 on esp32-wroom-smp.
 *
 * FRAME + 768 = 1024, and above one core FRAME + 832 = 1088: the KICKOS_MIN_STACK_SIZE floor. */
#if KICKOS_KERNEL_CORES > 1
#define KICKOS_LX6_TRAP_DEPTH 832
#else
#define KICKOS_LX6_TRAP_DEPTH 768
#endif

/* The panic reporter's own stack. FRAME is 0: only a fault inside the reporter stacks one here,
 * a bug the tests catch.
 *
 * 512 on esp32-wroom-smp, 336 on esp32-wroom.
 * kickos_panic_stack_enter spends 32 bytes on the stack it leaves, the windowed ABI giving it
 * no way to write a1 without opening a frame; trap_redzone_roots.txt declares that cost. */
#define KICKOS_LX6_PANIC_FRAME 0
#define KICKOS_LX6_PANIC_DEPTH 768

/* idle_entry and arch_idle_wait, whose frames sit above the level-1 interrupt idle waits in:
 * 64 on every registered preset. Enforced at 128. */
#define KICKOS_LX6_TRAP_DEPTH_IDLE 128

/* THE BOOT STACKS, one KICKOS_LX6_CORE_STACK below _estack per core, which arch_xtensa.cc
 * asserts holds both classes. DEPTH is the deepest root of each level the roots file declares,
 * summed, and a fault is charged one trap frame, as FAULT charges it.
 *
 * MAIN, the PRO_CPU's boot at INTLEVEL 15 with a fault nested on it: 1760 on esp32-wroom-smp,
 * the boot check's print waking a console writer, 1520 on esp32-wroom.
 *
 * CORE, the APP_CPU's park, a level-1 interrupt under its trap frame, and a fault in that, so
 * two trap frames: 1824 on esp32-wroom-smp.
 *
 * Both are reserved at 2560. */
#define KICKOS_LX6_CORE_FRAME 512
#define KICKOS_LX6_MAIN_DEPTH 2560
#define KICKOS_LX6_CORE_DEPTH 2560

/*
 * The trap classes tests/static/check_trap_redzone.sh measures against the figures above,
 * one line each; tests/static/trap_redzone_roots.txt says what each option means and roots
 * every class.
 *
 * class PREEMPT frame=KICKOS_LX6_TRAP_FRAME depth=KICKOS_LX6_TRAP_DEPTH
 * class FAULT   frame=KICKOS_LX6_TRAP_FRAME depth=KICKOS_LX6_TRAP_DEPTH
 * class PANIC   frame=KICKOS_LX6_PANIC_FRAME depth=KICKOS_LX6_PANIC_DEPTH stack=panic
 * class IDLE    frame=KICKOS_LX6_TRAP_FRAME depth=KICKOS_LX6_TRAP_DEPTH_IDLE
 * class MAIN    frame=KICKOS_LX6_TRAP_FRAME depth=KICKOS_LX6_MAIN_DEPTH stack=main size=KICKOS_LX6_CORE_STACK
 * class CORE    frame=KICKOS_LX6_CORE_FRAME depth=KICKOS_LX6_CORE_DEPTH stack=main size=KICKOS_LX6_CORE_STACK core=secondary
 */

#endif
