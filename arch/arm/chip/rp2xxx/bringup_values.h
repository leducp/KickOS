// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The values the RP2xxx bring-up computes, free of the chips' register headers so a host unit
// test can pin them: the system timer's tick divisor and the pad side of a pin function.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2XXX_BRINGUP_VALUES_H
#define KICKOS_ARCH_ARM_CHIP_RP2XXX_BRINGUP_VALUES_H

#include <stdint.h>

namespace kickos::rp2xxx
{
    // arch_clock_now reads the system timer as microseconds.
    constexpr uint32_t TICK_HZ = 1000000u;
    // CYCLES is 9 bits on both parts (RP2040 WATCHDOG TICK, RP2350 DS 8.5.1).
    constexpr uint32_t TICK_CYCLES_MAX = 0x1ffu;

    // The tick generator's divisor of clk_ref at `ref_hz`, rounded to the nearest.
    constexpr uint32_t tick_cycles(uint32_t ref_hz)
    {
        uint32_t cycles = (ref_hz + (TICK_HZ / 2u)) / TICK_HZ;
        if (cycles == 0u)
        {
            cycles = 1u;
        }
        if (cycles > TICK_CYCLES_MAX)
        {
            cycles = TICK_CYCLES_MAX;
        }
        return cycles;
    }

    // KOS_SYS_PINMUX_SET's func word: [4:0] the IO_BANK0 funcsel, PIN_INPUT sets the pad's input
    // enable, PIN_DRIVE clears its output disable, PIN_SIO_OUT enables the SIO output.
    constexpr uint32_t PIN_FUNCSEL_MASK = 0x1fu;
    constexpr uint32_t PIN_INPUT = 1u << 8;
    constexpr uint32_t PIN_DRIVE = 1u << 9;
    constexpr uint32_t PIN_SIO_OUT = 1u << 16;

    // The pad bits `func` clears in one store: the part's `release` bits always, its output
    // disable `od` on PIN_DRIVE.
    constexpr uint32_t pad_clear(uint32_t func, uint32_t release, uint32_t od)
    {
        uint32_t clr = release;
        if ((func & PIN_DRIVE) != 0u)
        {
            clr |= od;
        }
        return clr;
    }
}

#endif
