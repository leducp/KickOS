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
// arch calls back into kernel thread-exit (kickos_thread_return()).
void arch_context_init(struct arch_context* ctx,
                       void (*entry)(void* arg), void* arg,
                       void* stack_base, size_t stack_size,
                       int privileged);

// Switch the running context from `from` to `to`. MAY be deferred: on ARM this
// pends PendSV and the register swap happens on exception return; on the sim it
// happens now, or on interrupt-exit when called from ISR context. The scheduler
// must not assume the switch has completed when this returns.
void arch_switch(struct arch_context* from, struct arch_context* to);

// Enter the first thread from the boot context. `boot` is an optional save slot
// for the boot context: a backend MAY populate it (the sim does, so a later
// switch back unwinds to the host caller) or MAY ignore it and abandon the boot
// stack (the ARM backend does -- the system always terminates via arch_shutdown,
// never by unwinding to boot). Callers MUST NOT switch back to `boot`.
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

// --- Trace clock (telemetry timestamp seam) --------------------------------
// A dedicated high-resolution monotonic counter for telemetry timestamps: the
// ns arch_clock_now is too coarse to time a context switch (~1-5 us). u32 by
// design (wraps; the decoder reconstructs absolute time from the SESSION
// anchors). Per-arch source: armv7m = DWT CYCCNT (cycles); armv6m/chip-provided
// (rp2040 = the 1 MHz TIMER low half; nrf51 = semihosting us); sim = clock_now
// scaled to us. A target that has no such source does NOT define
// KICKOS_HAVE_TRACE_CLOCK and cannot enable telemetry (build-time FATAL).
uint32_t arch_trace_now(void);

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
// Stamp the owning thread's trace id into a saved context, so the arch context-
// switch path can emit {from,to} tids read from the PHYSICALLY-swapped contexts
// -- never by re-reading shared scheduler state (which an ISR can rewrite between
// the switch decision and the physical swap). Telemetry-only: this seam does not
// exist when telemetry is compiled out (the id field is elided too). Called once
// per thread in thread_create.
void arch_trace_stamp_id(struct arch_context* ctx, uint16_t id);
#endif

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
// set). sim: mprotect over the user-RAM arena -- grant the listed regions,
// everything else no-access. Regions are non-overlapping; attr is the
// UNPRIVILEGED access (supervisor comes from the background region / SYSMPU RGD0).
void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n);

// The smallest region this arch's MPU can enforce -- a global hardware property,
// NOT a per-region field (which would break the frozen arch_mpu_region seam):
// ARM PMSA 32 bytes, RISC-V PMP NAPOT 8, one host page on the sim (mprotect
// granularity). Region sizes floor to this so every descriptor is representable.
size_t arch_mpu_min_region(void);

// Round `want` up to the region geometry EVERY backend can describe with one
// descriptor: a power of two, at least arch_mpu_min_region(). The block
// arch_ram_alloc returns is sized and naturally aligned to this, so PMSA/NAPOT
// can cover it; the kernel sizes each thread/domain region descriptor with the
// SAME call, so the descriptor matches the backing block exactly.
static inline size_t arch_ram_region_size(size_t want)
{
    size_t min = arch_mpu_min_region();
    if (want < min)
    {
        want = min;
    }
    size_t p = 1;
    while (p < want)
    {
        size_t next = p << 1;
        if (next < p) // size_t overflow: unroundable, hand back the raw request
        {
            return want;
        }
        p = next;
    }
    return p;
}

// The MPU-governed user-RAM pool. Domain data + unprivileged-thread stacks are
// placed here so per-domain isolation is enforceable. sim: an mmap arena; MCU: a
// linker-defined region. arch_ram_alloc reserves a block sized by
// arch_ram_region_size() and NATURALLY ALIGNED to that size, so exactly one MPU
// region covers it. Returns null on exhaustion or size 0.
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

// --- Interrupt controller (thin abstraction: mask / unmask / raise) --------
// Deliberately minimal -- no priority grouping, pending-vs-active, edge-vs-level,
// or tail-chaining; those are earned per-chip at M1 against real silicon. On ARM
// this backs onto the NVIC; on the sim, signal-driven injection.
//
// mask/unmask gate delivery of a line. The generic first-level ISR masks the
// line before waking its driver (thread context), which unmasks via irq_ack once
// serviced -- so the line cannot re-fire while it is being handled. A raise of a
// masked line is suppressed (sim: dropped).
//
// RESET CONTRACT (uniform across every arch): all lines start MASKED at reset. A
// driver unmasks its line (arch_irq_unmask, or irq_register which arms it) before
// use; nothing may assume a line is deliverable until it has been unmasked.
void arch_irq_mask(int line);
void arch_irq_unmask(int line);

// Raise device line `irq` (the controller's "raise"). sim: delivers an async
// signal so the ISR runs in interrupt context; ARM: pends the NVIC line. Drives
// scheduler trigger #4. A real driver never raises -- it register/wait/acks;
// raising is fake-a-device-firing test scaffolding, privilege-gated.
void arch_irq_inject(int irq);

// --- Minimal debug console (bottom edge of the in-kernel console driver) ---
// Write-only. Two edges:
//   arch_console_write      -- normal path. A chip with a buffered console makes
//                              this enqueue + prime the TX IRQ (see console_tx.h);
//                              otherwise it is the polled writer.
//   arch_console_write_sync -- polled, bounded; safe with the scheduler/IRQs down.
//                              Panic / fault / assert / pre-arm output uses this.
//                              Weakly defaults to arch_console_write; a chip with a
//                              buffered console overrides it with its polled writer.
void arch_console_write(char const* buf, size_t n);
void arch_console_write_sync(char const* buf, size_t n);

// --- Single on-board kernel diagnostic LED (optional) ----------------------
// The board's one diagnostic LED -- the raw bottom edge of the kernel diag LED
// (kdiag_led_*), a sibling of the console: a last-resort self-debug facility
// that works UART-less, in a fault, before drivers exist. NOT a general device
// driver (the userspace path is provisional; the capability model re-homes it as
// a userspace GPIO driver later). arch_diag_led_init() configures the pin once
// at boot; arch_diag_led_set() drives it (on != 0). Both have a WEAK no-op
// default (kernel/init/led.cc): a board with no known LED -- or the sim -- does
// nothing; a chip backend with one provides strong overrides. Raw set (no
// toggle): the kernel side tracks state, so a toggle is one XOR there, not a
// per-chip register quirk.
void arch_diag_led_init(void);
void arch_diag_led_set(int on);

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

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
// A context switch physically completed: emit a SWITCH record {from_tid, to_tid}.
// The arch switch path calls this at the REAL register swap (ARM: the PendSV
// tail; sim: each ucontext swap site), from tids read out of the two contexts it
// actually swapped. from_tid == 0xFFFF on the very first switch. RESCAN group.
void kickos_trace_switch_done(uint16_t from_tid, uint16_t to_tid);

// Emit the closing SESSION record (final records_attempted + a second clock
// anchor). The sim backend calls this from arch_shutdown just before it drains
// the ch1 ring to a file, so the decoder gets its two-anchor resync span.
void kickos_trace_final_session(void);

// Print a one-line telemetry health report (attempted vs dropped) to the console
// at shutdown; a CI gate reads it to verify the drop accounting.
void kickos_trace_report_counters(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
