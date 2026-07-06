// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Kernel diagnostic LED: the board's single status LED, owned by the kernel as a
// sibling of the console (kernel/init/console.cc). It is a last-resort self-debug
// facility -- it works with no UART wired, inside a fault, before any driver
// exists -- NOT a general device driver. The kernel drives it directly for
// self-debug (a solid LED on panic); userspace borrows it through a syscall
// (kos_kernel_diag_led_*). That userspace path is PROVISIONAL: once the M2
// capability model lands, an app that blinks an LED becomes a userspace GPIO
// driver holding a device-memory capability, and only the kernel-side use stays.
//
// One physical pin, one owner: the kernel arbitrates so a panic indicator and a
// userspace heartbeat cannot fight over it. State is tracked here, so toggle()
// costs one XOR and the arch backend only has to implement a raw set().

#include <kickos/kernel.h>
#include <kickos/arch/arch.h>

// Weak no-op defaults for the raw bottom edge. A chip backend with a known LED
// provides strong overrides (arch/arm/chip/<chip>); a board without one -- or the
// sim -- links these and the LED silently does nothing. Strong beats weak at link
// regardless of archive order, so no ordering dependency.
extern "C" __attribute__((weak)) void arch_diag_led_init(void) {}
extern "C" __attribute__((weak)) void arch_diag_led_set(int) {}

namespace kickos
{
    namespace
    {
        bool g_led_on = false;
    }

    void kdiag_led_init(void)
    {
        arch_diag_led_init();
        g_led_on = false;
        arch_diag_led_set(0);
    }

    void kdiag_led_set(bool on)
    {
        g_led_on = on;
        arch_diag_led_set(on);
    }

    void kdiag_led_toggle(void)
    {
        kdiag_led_set(not g_led_on);
    }
}
