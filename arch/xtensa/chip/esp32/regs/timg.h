// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-D0WDQ6 Timer Group register map (ESP32 TRM ch.18 "Timer Group"). Offsets
// are from a group base (mmap::TIMG0_BASE or mmap::TIMG1_BASE); T0 owns
// +0x00..+0x20, T1 +0x24..+0x44, WDT +0x48+. T0 is the kernel monotonic time base;
// the two group MWDTs are disabled at bring-up.

#ifndef KICKOS_ARCH_XTENSA_CHIP_ESP32_REGS_TIMG_H
#define KICKOS_ARCH_XTENSA_CHIP_ESP32_REGS_TIMG_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::esp32::reg::timg
{
    // Group-relative offsets (apply to either TIMG0_BASE or TIMG1_BASE).
    constexpr uintptr_t T0CONFIG_OFF = 0x00u;
    constexpr uintptr_t T0LO_OFF = 0x04u;       // latched low 32 bits (RO)
    constexpr uintptr_t T0HI_OFF = 0x08u;       // latched high 32 bits (RO)
    constexpr uintptr_t T0UPDATE_OFF = 0x0Cu;   // write -> latch count to LO/HI
    constexpr uintptr_t T0LOADLO_OFF = 0x18u;
    constexpr uintptr_t T0LOADHI_OFF = 0x1Cu;
    constexpr uintptr_t T0LOAD_OFF = 0x20u;     // write -> counter <- {LOADHI,LOADLO}
    constexpr uintptr_t WDTCONFIG0_OFF = 0x48u;
    constexpr uintptr_t WDTWPROTECT_OFF = 0x64u;
    constexpr uintptr_t RTCCALICFG_OFF = 0x68u;

    // T0CONFIG fields. Free-running == INCREASE=1, AUTORELOAD=0, ALARM_EN=0.
    constexpr uint32_t T0_EN = 1u << 31;
    constexpr uint32_t T0_INCREASE = 1u << 30;
    constexpr uint32_t T0_AUTORELOAD = 1u << 29;
    constexpr uint32_t T0_ALARM_EN = 1u << 10;
    constexpr uint32_t T0_DIVIDER_SHIFT = 13; // 16-bit APB prescaler [28:13]

    // Prescaler config value. The divider field special-cases 0/1 on this silicon,
    // so 2 is the minimum unambiguous value.
    constexpr uint32_t DIVIDER = 2;

    // RTCCALICFG: CLK_SEL [14:13]=0 (RTC_SLOW), MAX [30:16]=0. RDY sets on the next
    // slow-clock edge; no self-clearing/ready quirk on the classic ESP32.
    constexpr uint32_t CALI_START = 1u << 31;
    constexpr uint32_t CALI_RDY = 1u << 15;

    // MWDT (WDTCONFIG0), unlocked by writing WDT_WKEY to WDTWPROTECT. Clearing
    // WDT_EN alone is not enough: FLASHBOOT_MOD_EN arms a separate flash-boot
    // watchdog. WDT_WKEY is shared with the RTC WDT (see regs/rtc_cntl.h).
    constexpr uint32_t WDT_WKEY = 0x50D83AA1u;
    constexpr uint32_t WDT_EN = 1u << 31;
    constexpr uint32_t WDT_FLASHBOOT_MOD_EN = 1u << 14;
}

#endif // KICKOS_ARCH_XTENSA_CHIP_ESP32_REGS_TIMG_H
