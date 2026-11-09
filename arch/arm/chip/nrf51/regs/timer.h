// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// nRF51 TIMER register map (nRF51 Series Reference Manual v3.0 ch.18), instanced for TIMER0,
// which the kernel's clock runs at 32 bits.

#ifndef KICKOS_ARCH_ARM_CHIP_NRF51_REGS_TIMER_H
#define KICKOS_ARCH_ARM_CHIP_NRF51_REGS_TIMER_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::nrf51::reg::timer
{
    // Tasks act on a write of 1 (sec.10.1.3).
    constexpr uintptr_t TASKS_START = mmap::TIMER0_BASE + 0x000u;
    constexpr uintptr_t TASKS_STOP = mmap::TIMER0_BASE + 0x004u;
    constexpr uintptr_t TASKS_CLEAR = mmap::TIMER0_BASE + 0x00Cu;
    constexpr uintptr_t TASKS_CAPTURE0 = mmap::TIMER0_BASE + 0x040u;
    // An event register clears only on a write of 0 (sec.10.1.4).
    constexpr uintptr_t EVENTS_COMPARE3 = mmap::TIMER0_BASE + 0x14Cu;
    constexpr uintptr_t INTENSET = mmap::TIMER0_BASE + 0x304u;
    constexpr uintptr_t MODE = mmap::TIMER0_BASE + 0x504u;
    constexpr uintptr_t BITMODE = mmap::TIMER0_BASE + 0x508u;
    constexpr uintptr_t PRESCALER = mmap::TIMER0_BASE + 0x510u;
    constexpr uintptr_t CC0 = mmap::TIMER0_BASE + 0x540u;
    constexpr uintptr_t CC3 = mmap::TIMER0_BASE + 0x54Cu;

    constexpr uint32_t INTEN_COMPARE3 = 1u << 19;
    constexpr uint32_t MODE_TIMER = 0u;
    constexpr uint32_t BITMODE_32 = 3u;
    constexpr uint32_t PRESCALER_1MHZ = 4u; // 16 MHz / 2^4
}

#endif
