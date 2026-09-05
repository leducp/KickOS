// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// The real-hardware dead-end: three 0.2 s blinks then a 2 s gap, forever, so a board
// with no console still says "panicked" at a glance. A chip that terminates through a
// debug channel (semihosting exit) defines its own. See kernel/include/kickos/kernel.h.
//
// The kernel-side declarations are repeated here rather than included: the arch layer
// deliberately carries no kernel/include path (arch/CMakeLists.txt).

#include <kickos/arch/arch.h>

#include <stdint.h>

namespace kickos
{
    void kdiag_led_set(bool on);
}

extern "C"
{
    void kpanic_enter(void);
    void kickos_bootloader_handover(void);
}

namespace
{
    // Wall-clock so the pattern is the SAME real duration on every board. A dead clock
    // (a pre-clock-init fault) never advances: probe for one tick and give up rather
    // than hang, then trust it.
    void panic_delay_ms(uint32_t ms)
    {
        uint64_t const start = arch_clock_now();
        uint64_t const span = static_cast<uint64_t>(ms) * 1000000ull;
        uint32_t probe = 0;
        while (arch_clock_now() == start)
        {
            probe++;
            if (probe >= (1u << 24)) // ~16M reads, no tick: clock is stopped
            {
                return;
            }
        }
        while (arch_clock_now() - start < span)
        {
        }
    }
}

extern "C" __attribute__((noreturn)) void kfault_terminate(void)
{
    // Before kpanic_enter, which reclaims the console and may reset the TX FIFO: a reporter's
    // bytes are still queued in that FIFO. Bounded, per arch.h.
    arch_console_flush_sync();
    kpanic_enter(); // idempotent; masks IRQs for any path reaching here directly
    kickos_bootloader_handover();
    while (true)
    {
        for (int b = 0; b < 3; b++)
        {
            ::kickos::kdiag_led_set(true);
            panic_delay_ms(200);
            ::kickos::kdiag_led_set(false);
            panic_delay_ms(200);
        }
        panic_delay_ms(2000); // 2 s dark gap before the next burst
    }
}
