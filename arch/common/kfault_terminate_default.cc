// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// The real-hardware dead-end: three 0.2 s blinks then a 2 s gap, forever. A chip that
// terminates through a debug channel defines its own.

#include <kickos/arch/arch.h>

#include <stdint.h>

extern "C"
{
    void kpanic_enter(void);
    void kickos_terminal_outcome_apply(void);
#if KICKOS_KERNEL_CORES > 1
    void kickos_doorbell_halt_peers(void);
#endif
}

namespace
{
    // A dead clock (a fault before clock init) never advances: probe for one tick and give up
    // rather than hang.
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
    kpanic_enter(); // idempotent; masks IRQs for a path reaching here directly
    kickos_terminal_outcome_apply();
#if KICKOS_KERNEL_CORES > 1
    kickos_doorbell_halt_peers();
#endif
    // The LED's pin may be a task's GPIO until here.
    arch_diag_led_init();
    while (true)
    {
        for (int b = 0; b < 3; b++)
        {
            arch_diag_led_set(1);
            panic_delay_ms(200);
            arch_diag_led_set(0);
            panic_delay_ms(200);
        }
        panic_delay_ms(2000);
    }
}
