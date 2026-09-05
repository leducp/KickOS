// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32 SoC clock-generation constants that are NOT a single MMIO
// peripheral: the BBPLL analog register file (reached over the internal reg-I2C
// bus, not memory-mapped), the ANA_CONFIG analog gate, and the fixed SoC clock
// rates. Register/analog facts are clean-room from the ESP32 TRM (RTC_CNTL + DPORT
// clock chapters, analog-PLL description). See mmap.h for ROM_REGI2C_WRITE.

#ifndef KICKOS_ARCH_XTENSA_CHIP_ESP32_REGS_SYSTEM_H
#define KICKOS_ARCH_XTENSA_CHIP_ESP32_REGS_SYSTEM_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::esp32::reg::system
{
    // What this chip's own bring-up programs. APB_CLOCK_HZ HOLDS ONLY WHILE SOC_CLK_SEL
    // SELECTS THE PLL, where it is 80 MHz for every CPUPERIOD_SEL (TRM v5.8 Table 7.2-4
    // p.169); off the PLL, APB follows the crystal, which no register on this part reports.
    // Read the select before using it as a rate (arch_periph_clock_hz).
    constexpr uint32_t CPU_CLOCK_HZ = 240000000u;
    constexpr uint32_t APB_CLOCK_HZ = 80000000u;

    // ANA_CONFIG analog gate: bits [17:8] gate the analog blocks; bit 17 = BBPLL.
    constexpr uintptr_t ANA_CONFIG = mmap::SYSCON_BASE + 0x44u;
    constexpr uint32_t ANA_CONFIG_ALL_GATES = 0x3FFu << 8;
    constexpr uint32_t ANA_CONFIG_BBPLL_GATE = 1u << 17;

    // BBPLL over reg-I2C (ROM_REGI2C_WRITE): analog block id 0x66, host id 4.
    constexpr uint8_t I2C_BBPLL = 0x66;
    constexpr uint8_t I2C_BBPLL_HOSTID = 4;
}

#endif
