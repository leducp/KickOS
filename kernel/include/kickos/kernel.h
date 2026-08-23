// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_KERNEL_H
#define KICKOS_KERNEL_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/diag.h>
#include <kickos/thread.h>

namespace kickos
{
    // Kernel entry: called by the arch boot path (sim: host main) after arch_init.
    // Creates the idle + root threads and starts the scheduler; the host argv is
    // forwarded to the app entry (argc=0/argv=nullptr on MCU). Does not return in
    // practice: arch_shutdown ends the process, and the int return is a formality.
    int kmain(int argc, char** argv);

    // Console fan-out: sends text to every enabled backend (KICKOS_CONSOLE =
    // chip|rtt|both|none). kputs/kprintf/kpanic and the console syscall route here.
    void kconsole_write(char const* buf, size_t n);

    // Debug console (in-kernel, write-only, unbuffered). Routes via kconsole_write.
    void kputs(char const* s);
    void kprintf(char const* fmt, ...) __attribute__((format(printf, 1, 2)));

    // kprintf for the thread-fault record ONLY. A published console makes the kernel chip
    // path a DROP, so this one additionally hands the line to the console driver
    // (cap_console_deliver) when a userspace driver owns the device. Widening it to kprintf
    // would relight the kernel debug console post-handover, which check_sim_published.sh
    // asserts is dark to prove the handover happened at all.
    void kprintf_fault(char const* fmt, ...) __attribute__((format(printf, 1, 2)));

    // Unrecoverable error: report and halt the system.
    void kpanic(char const* msg) __attribute__((noreturn));

#if KICKOS_DIAG_TERSE
    // The terse assert terminal. file and line arrive SEPARATELY on purpose: the file
    // string is then one literal per translation unit that every assert in it shares,
    // and the line never becomes a string at all. Joining them into one "file:line"
    // literal per site costs four times as much (M4.7.9_footprint_meas.md).
    void kpanic_at(char const* file, unsigned line) __attribute__((noreturn));
#endif

    // Kernel diagnostic LED: the board's single status LED, a sibling of the
    // console. init() at boot; set()/toggle() drive it. Owned by the kernel so a
    // panic indicator and a userspace heartbeat (kos_kernel_diag_led_*) share one
    // pin without fighting. No-op on boards with no known LED. State is tracked
    // here so toggle() needs no per-chip toggle register.
    void kdiag_led_init(void);
    void kdiag_led_set(bool on);
    void kdiag_led_toggle(void);

    // Build a thread. `stack_base`/`stack_size` and the TCB storage are supplied by the
    // caller (static allocation first). Leaves it INACTIVE: the caller publishes it with
    // sched::add once it has paid the unmasked part of the setup (kickos_reent_init).
    // Nothing may make it READY before that, or it can be picked half-built.
    void thread_create(Thread* t, void (*entry)(void*), void* arg,
                       void* stack_base, size_t stack_size, ThreadAttr const& attr);

    // True iff NO live thread holds a DEV region overlapping [base, base+size). The
    // admission test behind the one-holder-per-window rule, and ALSO the console reclaim's
    // precondition: re-initialising a device whose window a live thread still holds corrupts
    // it under that thread. A DYING thread is not a holder, which is what lets a supervisor
    // woken by the teardown's EPIPE respawn into the window at once. Callers pass a
    // non-wrapping window.
    bool dev_window_free(uintptr_t base, size_t size);

    // Break `t` out of whatever it is parked on and hand it `result`, without granting it
    // whatever it was waiting for. Every WaitKind is covered, so a park is never a place a
    // thread cannot be reached. Caller holds IrqLock and `t` must be BLOCKED.
    void thread_abort_park(Thread* t, intptr_t result);

    // Raise `t`'s cancellation to `kind` and, if it is parked, break that park so it becomes
    // runnable. `kind` is a CancelKind and decides what happens then: CANCEL_KILL leaves the
    // thread its cleanup window and ends it at its next syscall entry, CANCEL_SLAY claims its
    // next resume in switch_to and it executes no further unprivileged instruction.
    //
    // MONOTONIC in the enum's VALUES: a kind at or below the one already recorded is
    // discarded, so a later kill can never demote a slay. That is what keeps ONE authority
    // answering "has this thread been asked to die, and how" instead of a second flag.
    //
    // Idempotent, and a no-op on a thread that is already dead or dying. Caller holds IrqLock.
    void thread_cancel_kind(Thread* t, uint8_t kind);
    // thread_cancel_kind(t, CANCEL_KILL): the cooperative form, and the only one before slay.
    void thread_cancel(Thread* t);
}

// Enter the panic / fault dead-end. Called FIRST by kpanic and by every arch fault
// reporter, before any dump is printed. Three premise-free steps, in order:
//   1. mask IRQs on this core (arch_irq_save, never restored, since we do not return),
//      so the timer/scheduler/other threads stop while the dump prints and the
//      terminal blinks (kpanic runs in THREAD context; the fault path is already
//      masked, where this is a harmless re-mask);
//   2. force the console onto the synchronous polled writer for all subsequent
//      output, whether or not this arch ever armed the buffered ring;
//   3. drain bytes already queued in the ring so the dump prints in order.
// Idempotent: safe to call again from kfault_terminate after a reporter called it.
extern "C" void kpanic_enter(void);

// Terminal for the panic / fault dead-end, shared by kpanic and the arch fault
// handlers. The fallback (arch/common/kfault_terminate_default.cc) blinks the diag LED in a distinctive
// pattern forever, which is the only signal available on real headless hardware. The host
// and QEMU targets override it (sim.cc / chip_mps2 / chip_virt / chip_nrf51) to exit
// with a fault status, so the test harness catches a fault instead of timing out
// on a spin. extern "C": overridden across TUs and called from the arch handlers.
// Under KICKOS_SHUTDOWN_TO_BOOTLOADER the fallback hands the chip to its
// bootloader instead of reaching the blink. An ARM MPU/hard fault terminates HERE
// (kickos_armv7m_fault_report), never through kickos_terminate, so a chip that would
// otherwise need a physical boot-select press needs this arm and not only the one below.
extern "C" void kfault_terminate(void) __attribute__((noreturn));

// The chokepoint every ORDERED terminal path goes through: the KOS_SYS_SHUTDOWN
// syscall, KOS_SYS_EXIT issued by ROOT, last-thread-out (sched::exit_current) and the
// software fault reporter (kickos_isr_fault, the RISC-V/chip-hook route). Drains the
// buffered console, then ends the system via arch_shutdown. Sits here, upstream of the
// arch seam, because arch_shutdown itself is per-chip (one fallback TU plus four chip
// backends); a hook inside it would have to be duplicated per arch.
// Under KICKOS_SHUTDOWN_TO_BOOTLOADER it tries arch_reboot before halting, so a
// bench board returns to a flashable state on its own. A chip with no bootloader
// entry declines with -KOS_ENOSYS and falls through to the halt.
extern "C" void kickos_terminate(int status) __attribute__((noreturn));

#if KICKOS_DIAG_TERSE
#define KICKOS_ASSERT(cond)                               \
    do                                                    \
    {                                                     \
        if (not(cond))                                    \
        {                                                 \
            ::kickos::kpanic_at(__FILE_NAME__, __LINE__); \
        }                                                 \
    } while (0)
#else
#define KICKOS_ASSERT(cond)                     \
    do                                          \
    {                                           \
        if (not(cond))                          \
        {                                       \
            ::kickos::kpanic("assert: " #cond); \
        }                                       \
    } while (0)
#endif

// A control-flow point that must never be reached: halt LOUDLY with a diagnostic
// (kpanic is [[noreturn]]), never spin silently. Distinct from a defensive guard
// (e.g. the kernel().live clamp), which prevents a real consequence and stays.
// Takes a diag.h catalogue entry, which carries its own "unreachable: " in the prose
// column, because a runtime pointer cannot be concatenated with a prefix here.
// It expands to a bare kpanic and is NOT therefore redundant: it marks a state the code
// believes cannot occur, where kpanic marks one it refuses to continue from. Keeping the
// two spellings apart is what makes the first kind greppable.
#define KICKOS_UNREACHABLE(msg) ::kickos::kpanic(msg)

#endif
