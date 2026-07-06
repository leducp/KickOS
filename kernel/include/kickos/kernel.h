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
