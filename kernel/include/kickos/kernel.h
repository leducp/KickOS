// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_KERNEL_H
#define KICKOS_KERNEL_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/thread.h>

namespace kickos
{
    // Kernel entry: called by the arch boot path (sim: host main) after arch_init.
    // Creates the idle + root threads and starts the scheduler; the host argv is
    // forwarded to the app entry (argc=0/argv=nullptr on MCU). Does not return in
    // practice -- arch_shutdown ends the process; the int return is a formality.
    int kmain(int argc, char** argv);

    // Console fan-out: sends text to every enabled backend (KICKOS_CONSOLE =
    // chip|rtt|both|none). kputs/kprintf/kpanic and the console syscall route here.
    void kconsole_write(char const* buf, size_t n);

    // Debug console (in-kernel, write-only, unbuffered). Routes via kconsole_write.
    void kputs(char const* s);
    void kprintf(char const* fmt, ...) __attribute__((format(printf, 1, 2)));

    // Unrecoverable error: report and halt the system.
    void kpanic(char const* msg) __attribute__((noreturn));

    // Kernel diagnostic LED: the board's single status LED, a sibling of the
    // console. init() at boot; set()/toggle() drive it. Owned by the kernel so a
    // panic indicator and a userspace heartbeat (kos_kernel_diag_led_*) share one
    // pin without fighting. No-op on boards with no known LED. State is tracked
    // here so toggle() needs no per-chip toggle register.
    void kdiag_led_init(void);
    void kdiag_led_set(bool on);
    void kdiag_led_toggle(void);

    // Create a thread. `stack_base`/`stack_size` and the TCB storage are supplied
    // by the caller (static allocation first). Adds it as READY.
    void thread_create(Thread* t, void (*entry)(void*), void* arg,
                       void* stack_base, size_t stack_size, ThreadAttr const& attr);
}

// Enter the panic / fault dead-end. Called FIRST by kpanic and by every arch fault
// reporter, before any dump is printed. Three premise-free steps, in order:
//   1. mask IRQs on this core (arch_irq_save, never restored -- we do not return),
//      so the timer/scheduler/other threads stop while the dump prints and the
//      terminal blinks (kpanic runs in THREAD context; the fault path is already
//      masked, where this is a harmless re-mask);
//   2. force the console onto the synchronous polled writer for all subsequent
//      output -- works whether or not this arch armed the buffered ring, which
//      retires the whole "did this arch arm the ring?" class of fault-path bugs;
//   3. drain bytes already queued in the ring so the dump prints in order.
// Idempotent: safe to call again from kfault_terminate after a reporter called it.
extern "C" void kpanic_enter(void);

// Terminal for the panic / fault dead-end, shared by kpanic and the arch fault
// handlers. The weak default (console.cc) blinks the diag LED in a distinctive
// pattern forever -- the right signal on real, headless hardware. The host and
// QEMU targets override it (sim.cc / chip_mps2 / chip_virt / chip_nrf51) to exit
// with a fault status, so the test harness catches a fault instead of timing out
// on a spin. extern "C": overridden across TUs and called from the arch handlers.
extern "C" void kfault_terminate(void) __attribute__((noreturn));

#define KICKOS_ASSERT(cond)                     \
    do                                          \
    {                                           \
        if (not(cond))                          \
        {                                       \
            ::kickos::kpanic("assert: " #cond); \
        }                                       \
    } while (0)

// A control-flow point that must never be reached: halt LOUDLY with a diagnostic
// (kpanic is [[noreturn]]), never spin silently. Distinct from a defensive guard
// (e.g. the kernel().live clamp), which prevents a real consequence and stays.
#define KICKOS_UNREACHABLE(msg) ::kickos::kpanic("unreachable: " msg)

#endif
