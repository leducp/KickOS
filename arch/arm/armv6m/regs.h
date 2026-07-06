// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Minimal ARMv6-M (Cortex-M0/M0+) core register definitions -- arch-internal,
// clean-room (offsets from the ARMv6-M Architecture Reference Manual). Versus
// v7-M: NO BASEPRI (critical section is PRIMASK = mask all), NO DWT cycle
// counter (the monotonic clock is chip-provided), and SHPR/NVIC-IPR are
// word-access only (no byte writes).

#ifndef KICKOS_ARCH_ARMV6M_REGS_H
#define KICKOS_ARCH_ARMV6M_REGS_H

#include <stdint.h>

#include "../common/regs.h" // reg32, SCB_ICSR/ICSR_*, SysTick, NVIC (shared)

namespace kickos
{
    namespace armv6m
    {
        // --- System Control Block (arch-specific: SHPR is word-access only) ---
        constexpr uintptr_t SCB_SHPR2 = 0xE000ED1C; // SVCall (#11)
        constexpr uintptr_t SCB_SHPR3 = 0xE000ED20; // PendSV (#14) / SysTick (#15)

        // System-handler priorities (only the top 2 bits are implemented on
        // v6-M). PendSV lowest so it tail-chains after every other exception;
        // device IRQ priorities are irrelevant to the crit section (PRIMASK
        // masks all), unlike the v7-M BASEPRI band.
        constexpr uint32_t PRIO_SYSTICK = 0xC0;
        constexpr uint32_t PRIO_SVCALL = 0xC0;
        constexpr uint32_t PRIO_PENDSV = 0xC0;
    }
}

#endif
