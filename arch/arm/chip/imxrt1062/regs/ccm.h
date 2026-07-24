// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// NXP i.MX RT1062 CCM clock-gating register map (RM ch.14). Only the CCGR gate
// roots KickOS touches; the full ARM-PLL/AHB clock tree (RM 14.7/14.8) is left
// as the boot ROM configured it (clock_init() is deferred). The ROM handoff
// state (RM Table 9-7 "ROM Clock Setting") is what fixes AHB_CLK_ROOT at 396 MHz
// and uart_clk_root at 20 MHz; see regs/lpuart.h for the UART-clock derivation.

#ifndef KICKOS_ARCH_ARM_CHIP_IMXRT1062_REGS_CCM_H
#define KICKOS_ARCH_ARM_CHIP_IMXRT1062_REGS_CCM_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::imxrt1062::reg::ccm
{
    // Clock gating registers (RM 14.7). Each peripheral gate is a 2-bit CGn field.
    constexpr uintptr_t CCGR1 = mmap::CCM_BASE + 0x6Cu; // GPT1: CG10 [21:20], CG11 [23:22]
    constexpr uintptr_t CCGR3 = mmap::CCM_BASE + 0x74u; // LPUART6: CG3 [7:6]

    constexpr uint32_t CCGR3_LPUART6 = 3u << 6;
    constexpr uint32_t CCGR1_GPT1 = (3u << 20) | (3u << 22); // bus + serial, on
}

#endif // KICKOS_ARCH_ARM_CHIP_IMXRT1062_REGS_CCM_H
