// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// NXP i.MX RT1062 watchdog register maps: WDOG1/2 (RM ch.57, 16-bit access ONLY;
// a 32-bit access is illegal, RM 57.8.1) and RTWDOG/WDOG3 (RM ch.58, 32-bit).
// The ROM hands off an armed RTWDOG (RM 58.4); KickOS disables all three.

#ifndef KICKOS_ARCH_ARM_CHIP_IMXRT1062_REGS_WDOG_H
#define KICKOS_ARCH_ARM_CHIP_IMXRT1062_REGS_WDOG_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::imxrt1062::reg::wdog
{
    // WDOG1/2 power-down control (16-bit).
    constexpr uintptr_t WDOG1_WMCR = mmap::WDOG1_BASE + 0x08u;
    constexpr uintptr_t WDOG2_WMCR = mmap::WDOG2_BASE + 0x08u;

    // RTWDOG (WDOG3) 32-bit registers.
    constexpr uintptr_t RTWDOG_CS = mmap::RTWDOG_BASE + 0x00u;
    constexpr uintptr_t RTWDOG_CNT = mmap::RTWDOG_BASE + 0x04u;
    constexpr uintptr_t RTWDOG_TOVAL = mmap::RTWDOG_BASE + 0x08u;

    constexpr uint32_t RTWDOG_UNLOCK = 0xD928C520u; // RM 58.3.2.2.1 unlock key
    constexpr uint32_t RTWDOG_CS_EN = 1u << 7;
    constexpr uint32_t RTWDOG_CS_RCS = 1u << 10; // reconfig-success flag
}

#endif // KICKOS_ARCH_ARM_CHIP_IMXRT1062_REGS_WDOG_H
