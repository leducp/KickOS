// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// THE KickOS porting interface. Every target implements exactly this seam.
//
// ISA-neutral by design: it names *concepts* (switch, crit-section, timer,
// mpu, syscall), never *mechanisms*. PendSV/SVC/BASEPRI live inside arch/arm,
// never here. Litmus test: a non-ARM port (Renesas RX72M) must fit this seam
// with no signature changes.

#ifndef KICKOS_ARCH_ARCH_H
#define KICKOS_ARCH_ARCH_H

#include <stddef.h>
#include <stdint.h>

// Per-arch definition of `struct arch_context` (opaque to the kernel; sized by
// the arch). Resolved to arch/<arch>/include/kickos/arch/context.h.
#include <kickos/arch/context.h>

#ifdef __cplusplus
extern "C"
{
#endif

// --- One-time backend bring-up ---------------------------------------------
// sim: install signal handlers, create the interval timer, map the RAM arena.
void arch_init(void);

// Terminate the whole system with the given process/exit status. On the sim
// this ends the host process; on MCUs it halts.
void arch_shutdown(int status) __attribute__((noreturn));

// --- Context / switching ---------------------------------------------------
// Build an initial frame in `ctx` so the first switch-in "returns" into
// entry(arg) on [stack_base, stack_base+stack_size). `privileged` selects the
// kernel (privileged) vs user (unprivileged) posture. When `entry` returns the
// arch calls back into kernel thread-exit (arch_thread_trampoline_exit()).
void arch_context_init(struct arch_context* ctx,
                       void (*entry)(void* arg), void* arg,
                       void* stack_base, size_t stack_size,
                       int privileged);

// Switch the running context from `from` to `to`. MAY be deferred: on ARM this
// pends PendSV and the register swap happens on exception return; on the sim it
// happens now, or on interrupt-exit when called from ISR context. The scheduler
// must not assume the switch has completed when this returns.
void arch_switch(struct arch_context* from, struct arch_context* to);

// Enter the first thread from the boot/host context. `boot` receives the boot
// context so a later switch back to it unwinds to the caller (system shutdown).
void arch_start(struct arch_context* boot, struct arch_context* first);

// --- Critical section (RAII-wrapped by kernel IrqLock) ---------------------
typedef uintptr_t arch_irq_state_t;
arch_irq_state_t arch_irq_save(void);
void arch_irq_restore(arch_irq_state_t state);

// Nonzero while executing in interrupt/ISR context.
int arch_in_isr(void);

// --- Tickless clock + one-shot next-event timer ----------------------------
uint64_t arch_clock_now(void); // monotonic nanoseconds
void arch_timer_arm(uint64_t deadline_ns);
void arch_timer_disarm(void);

// --- MPU: per-task memory protection ---------------------------------------
enum
{
    ARCH_MPU_NONE = 0,
    ARCH_MPU_R = 1u << 0,
    ARCH_MPU_W = 1u << 1,
    ARCH_MPU_X = 1u << 2,
    ARCH_MPU_DEV = 1u << 3 // device / MMIO
};

struct arch_mpu_region
{
    uintptr_t base;
    size_t size;
    uint32_t attr; // OR of the ARCH_MPU_* bits
};

// Load the running thread's regions on switch-in (replaces the whole active
// set). sim: mprotect over the user-RAM arena — grant the listed regions,
// everything else no-access. Regions must be page-aligned and non-overlapping.
void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n);

// The MPU-governed user-RAM pool. Domain data + (later) task stacks are placed
// here so per-domain isolation is enforceable. sim: an mmap arena; MCU: a
// linker-defined region. arch_ram_alloc bump-allocates page-aligned blocks.
uintptr_t arch_ram_base(void);
size_t arch_ram_size(void);
void* arch_ram_alloc(size_t size);

// An address that faults on unprivileged access (sim: a reserved arena page no
// domain owns). Used by the isolation self-test.
uintptr_t arch_mpu_probe_addr(void);

// --- Syscall trap (user -> kernel) -----------------------------------------
// Issued by the userspace syscall stubs; returns the syscall result.
//
// CONTRACT (portability-critical): the arch MUST run syscall_dispatch() in
// privileged THREAD context on the calling thread's own continuation -- NOT in
// ISR/handler context. A blocking syscall (sem_wait, sleep, ...) blocks by an
// ordinary synchronous context switch (arch_switch completes, resuming the
// dispatch inline when the thread is next scheduled), exactly as if the kernel
// routine were called directly. The kernel's blocking primitives depend on
// this; arch_in_isr() must read false during dispatch.
//   sim: arch_syscall is a plain call, so dispatch already runs in thread
//        context and arch_switch swaps synchronously.
//   ARM (later): SVC raises privilege and continues dispatch in privileged
//        thread mode (so a blocking switch/PendSV saves the mid-dispatch
//        continuation and resumes it), rather than running dispatch in the SVC
//        handler where a switch could only be deferred.
// 64-bit arguments/results are split into uintptr_t halves (see sys/abi.h),
// so no arch-specific result-delivery seam is needed.
uintptr_t arch_syscall(uintptr_t nr,
                       uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3);

// --- Emulated device interrupts (sim) --------------------------------------
// Raise device line `irq`. sim: delivers an async signal so the ISR runs in
// interrupt context; ARM: pends the NVIC line. Drives scheduler trigger #4.
void arch_irq_inject(int irq);

// --- Minimal debug console (bottom edge of the in-kernel console driver) ---
// Write-only, unbuffered. sim: host stdout; MCU: polled UART.
void arch_console_write(char const* buf, size_t n);

// --- Idle -------------------------------------------------------------------
// Block until the next interrupt (ARM WFI; sim sigsuspend).
void arch_idle_wait(void);

// --- Provided by the kernel, called back by the arch backend ---------------
// Next-event timer expired (tickless deadline or, if enabled, periodic tick).
void kickos_isr_timer(void);
// Device interrupt line `irq` fired (sim: injected event; ARM: NVIC line).
void kickos_isr_irq(int irq);
// A thread's entry function returned; the arch trampoline routes here.
void kickos_thread_return(void) __attribute__((noreturn));
// The arch-independent syscall table dispatch (called by arch_syscall / SVC).
uintptr_t syscall_dispatch(uintptr_t nr,
                           uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3);
// A memory-protection violation was caught (sim: SIGSEGV over the arena).
void kickos_isr_fault(uintptr_t addr, int is_write);

#ifdef __cplusplus
}
#endif

#endif
