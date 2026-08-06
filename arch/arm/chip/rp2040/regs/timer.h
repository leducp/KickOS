// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2040 TIMER register map (RP2040 datasheet, RP-008371-DS, 4.6): a 64-bit
// microsecond monotonic counter. The RAW halves do NOT latch, so a hi/lo/hi
// re-read stays core-safe without an IRQ guard (the latching TIMELR/TIMEHR pair is
// single-core only).

#ifndef KICKOS_ARCH_ARM_CHIP_RP2040_REGS_TIMER_H
#define KICKOS_ARCH_ARM_CHIP_RP2040_REGS_TIMER_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::rp2040::reg::timer
{
    constexpr uintptr_t TIMERAWH = mmap::TIMER_BASE + 0x24u; // raw high half
    constexpr uintptr_t TIMERAWL = mmap::TIMER_BASE + 0x28u; // raw low half (1 MHz)
}

#endif // KICKOS_ARCH_ARM_CHIP_RP2040_REGS_TIMER_H
