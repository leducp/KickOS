// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The semihosting console, clock and exit of the chips QEMU runs with -semihosting. A chip
// takes the console seams by compiling semihost_console.cc into its own archive (family.cmake).

#ifndef KICKOS_ARCH_COMMON_SEMIHOST_H
#define KICKOS_ARCH_COMMON_SEMIHOST_H

#include <kickos/arch/arch.h>

#include <stdint.h>

#include "semihost_trap.h"

namespace kickos::semihost
{
    constexpr long SYS_WRITEC = 0x03;
    constexpr long SYS_CLOCK = 0x10;
    constexpr long SYS_EXIT_EXTENDED = 0x20;
    constexpr uint32_t ADP_Stopped_ApplicationExit = 0x20026u;

    inline __attribute__((always_inline, noreturn)) void exit(int status)
    {
        uint32_t block[2];
        block[0] = ADP_Stopped_ApplicationExit;
        block[1] = static_cast<uint32_t>(status);
        call(SYS_EXIT_EXTENDED, block);
        halt_masked();
    }

    // SYS_CLOCK counts centiseconds, so 10 ms resolution. Clamped to `last` so an error return
    // cannot move the clock back and stall every armed sleeper. `last` is 64-bit and shared with
    // ISR context on a 32-bit core, so the clamp runs masked: a torn store it latched would jump
    // the clock forward for good. ALWAYS inlined: an MPS2 arch_trace_now reaches it on the
    // PendSV tail, which only that chip TU's own -mgeneral-regs-only keeps FP-register-free.
    inline __attribute__((always_inline)) uint64_t clock_now(uint64_t& last)
    {
        arch_irq_state_t st = arch_irq_save();
        long cs = call(SYS_CLOCK, nullptr);
        uint64_t ns = 0;
        if (cs > 0)
        {
            ns = static_cast<uint64_t>(cs) * 10000000ull;
        }
        if (ns < last)
        {
            ns = last;
        }
        last = ns;
        arch_irq_restore(st);
        return ns;
    }
}

#endif
