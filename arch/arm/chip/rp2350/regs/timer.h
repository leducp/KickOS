// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2350 TIMER0 register map (RP2350 datasheet RP-008373-DS-2, 12.8): 64-bit
// microsecond monotonic counter. The RAW halves do NOT latch, so a hi/lo/hi
// re-read is core-safe without an IRQ guard (unlike the latching TIMELR/TIMEHR
// pair). Offsets are TIMER0_BASE-relative (see mmap.h).

#ifndef KICKOS_ARCH_ARM_CHIP_RP2350_REGS_TIMER_H
#define KICKOS_ARCH_ARM_CHIP_RP2350_REGS_TIMER_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::rp2350::reg::timer
{
    constexpr uintptr_t TIMERAWH = mmap::TIMER0_BASE + 0x24u; // raw high half
    constexpr uintptr_t TIMERAWL = mmap::TIMER0_BASE + 0x28u; // raw low half
}

#endif // KICKOS_ARCH_ARM_CHIP_RP2350_REGS_TIMER_H
