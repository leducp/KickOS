// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The tail every arch_reboot shares once it has asked the hardware: the reset either lands, or
// the request is reported once and the system halts.

#ifndef KICKOS_ARCH_COMMON_ARCH_REBOOT_TAIL_H
#define KICKOS_ARCH_COMMON_ARCH_REBOOT_TAIL_H

#include <kickos/arch/arch.h>

#include <stddef.h>
#include <stdint.h>

#include <fatal_status.ld.h>

extern "C"
{
    void kpanic_enter_unlocked(void);
    void kpanic_write_unlocked(char const* buf, size_t n);
}

namespace kickos::arch
{
    // A bound in spins, not in time: some seconds at a 12 MHz boot clock, far longer than a
    // requested reset takes to land.
    constexpr uint32_t RESET_SETTLE_SPINS = 1u << 24;

    inline void reset_settle(void)
    {
        for (volatile uint32_t i = 0; i < RESET_SETTLE_SPINS; i = i + 1u)
        {
        }
    }

    // The console is drained before any reset request, so the unlocked entry drops no line.
    // Never kfault_terminate: it applies the terminal outcome, which re-enters arch_reboot.
    [[noreturn]] inline void reboot_failed(char const* line)
    {
        kpanic_enter_unlocked();
        kpanic_write_unlocked(line, __builtin_strlen(line));
        kpanic_write_unlocked("\n", 1u);
        arch_shutdown(KICKOS_FATAL_STATUS);
    }
}

#endif
