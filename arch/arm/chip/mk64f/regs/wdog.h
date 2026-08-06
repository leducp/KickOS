// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// MK64FN1M0 WDOG register map (K64 Sub-Family RM ch.24). 16-bit registers.

#ifndef KICKOS_ARCH_ARM_CHIP_MK64F_REGS_WDOG_H
#define KICKOS_ARCH_ARM_CHIP_MK64F_REGS_WDOG_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::mk64f::reg::wdog
{
    constexpr uintptr_t STCTRLH = mmap::WDOG_BASE + 0x00u; // 16-bit
    constexpr uintptr_t UNLOCK = mmap::WDOG_BASE + 0x0Eu;  // 16-bit

    // Unlock keys (RM 24.3.1): both stores must land within 20 bus cycles.
    constexpr uint16_t UNLOCK_KEY_1 = 0xC520u;
    constexpr uint16_t UNLOCK_KEY_2 = 0xD928u;

    // STCTRLH reset value 0x01D3 with WDOGEN cleared, keeping ALLOWUPDATE and the
    // reset-1 reserved bit 8 (matches NXP SystemInit).
    constexpr uint16_t STCTRLH_DISABLE = 0x01D2u;
}

#endif // KICKOS_ARCH_ARM_CHIP_MK64F_REGS_WDOG_H
