// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-D0WDQ6 RTC_CNTL register map (ESP32 TRM RTC_CNTL + clock chapters):
// core-voltage / analog power-down / CPU clock source select, and the RTC
// watchdog. The classic ESP32 has NO RTC super-watchdog (SWD); that block is an
// ESP32-S2+ addition.

#ifndef KICKOS_ARCH_XTENSA_CHIP_ESP32_REGS_RTC_CNTL_H
#define KICKOS_ARCH_XTENSA_CHIP_ESP32_REGS_RTC_CNTL_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::esp32::reg::rtc_cntl
{
    constexpr uintptr_t OPTIONS0 = mmap::RTC_CNTL_BASE + 0x00u;
    constexpr uintptr_t CLK_CONF = mmap::RTC_CNTL_BASE + 0x70u;
    constexpr uintptr_t DBIAS_REG = mmap::RTC_CNTL_BASE + 0x7Cu; // core-voltage (dbias)
    constexpr uintptr_t WDTCONFIG0 = mmap::RTC_CNTL_BASE + 0x8Cu;
    constexpr uintptr_t WDTWPROTECT = mmap::RTC_CNTL_BASE + 0xA4u;

    // OPTIONS0 analog force-power-down bits (clear to power the block up).
    constexpr uint32_t BB_I2C_FORCE_PD = 1u << 6;
    constexpr uint32_t BBPLL_I2C_FORCE_PD = 1u << 8;
    constexpr uint32_t BBPLL_FORCE_PD = 1u << 10;
    constexpr uint32_t BIAS_I2C_FORCE_PD = 1u << 18; // internal reg-I2C bus PD

    // DBIAS: RTC_CNTL_DIG_DBIAS_WAK [13:11]. 1V25 is required for 240 MHz.
    constexpr uint32_t DIG_DBIAS_SHIFT = 11;
    constexpr uint32_t DIG_DBIAS_MASK = 0x7u;
    constexpr uint32_t DIG_DBIAS_1V25 = 7;

    // CLK_CONF: RTC_CNTL_SOC_CLK_SEL [28:27]. 0=XTAL 1=PLL 2=CK8M 3=APLL.
    constexpr uint32_t SOC_CLK_SEL_SHIFT = 27;
    constexpr uint32_t SOC_CLK_SEL_MASK = 0x3u;
    constexpr uint32_t SOC_CLK_SEL_PLL = 1;

    // RTC watchdog (WDTCONFIG0), unlocked by writing WDT_WKEY to WDTWPROTECT.
    // FLASHBOOT_MOD_EN arms a separate flash-boot watchdog and must also be cleared.
    // WDT_WKEY / WDT_EN encoding is shared with the TIMG MWDTs (see regs/timg.h).
    constexpr uint32_t WDT_WKEY = 0x50D83AA1u;
    constexpr uint32_t WDT_EN = 1u << 31;
    constexpr uint32_t WDT_FLASHBOOT_MOD_EN = 1u << 10;
}

#endif // KICKOS_ARCH_XTENSA_CHIP_ESP32_REGS_RTC_CNTL_H
