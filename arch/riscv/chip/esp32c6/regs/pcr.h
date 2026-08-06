// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 PCR (Power/Clock/Reset) registers (TRM v1.2, PCR chapter). The C6's
// clock-gate + reset controller; on this core the peripheral clock source select +
// divider live here, not in the peripheral's own SYS_CONF. Only the RMT clock leaf
// is programmed so far (diag LED). PCR is an owns-for-life reserved MPU block.

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_PCR_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_PCR_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::esp32c6::reg::pcr
{
    constexpr uintptr_t RMT_CONF = mmap::PCR_BASE + 0x2Cu;      // CLK_EN bit0, RST_EN bit1
    constexpr uintptr_t RMT_SCLK_CONF = mmap::PCR_BASE + 0x30u;

    constexpr uint32_t RMT_CLK_EN = 1u << 0;
    constexpr uint32_t RMT_RST_EN = 1u << 1;
    constexpr uint32_t RMT_SCLK_EN = 1u << 22;
    constexpr uint32_t RMT_SCLK_SEL_S = 20u;     // [21:20] 1=PLL_F80M 2=RC_FAST 3=XTAL
    constexpr uint32_t RMT_SCLK_DIV_NUM_S = 12u; // [19:12] group divisor = value+1
}

#endif
