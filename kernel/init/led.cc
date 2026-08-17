// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Kernel diagnostic LED: the board's single status LED. Usable with no UART wired,
// inside a fault, and before any driver exists, which is why it is not a device driver.
// One physical pin, one owner: the kernel arbitrates, so a panic indicator and a
// userspace heartbeat (kos_kernel_diag_led_*) cannot fight over it. State is tracked
// here, so the arch backend implements only a raw set().

#include <kickos/kernel.h>
#include <kickos/arch/arch.h>

namespace kickos
{
    namespace
    {
        constinit bool g_led_on = false;
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
