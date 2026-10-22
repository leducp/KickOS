// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Kernel diagnostic LED, usable inside a fault and before any driver exists. One pin, one
// owner: the kernel arbitrates, so a panic indicator and a userspace heartbeat
// (kos_kernel_diag_led_set) cannot fight over it.

#include <kickos/kernel.h>
#include <kickos/arch/arch.h>

namespace kickos
{
    void kdiag_led_init(void)
    {
        arch_diag_led_init();
        arch_diag_led_set(0);
    }

    void kdiag_led_set(bool on)
    {
        arch_diag_led_set(on);
    }
}
