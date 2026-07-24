// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 watchdog registers (TRM v1.2 ch.14 MWDT in TIMG0/TIMG1, ch.15 RWDT/SWD in
// the LP domain). ALL must be disabled at boot or the ROM-armed WDTs reset the part
// within seconds. Each guards its config behind a write-protect key.

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_WDT_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_WDT_H

#include <stdint.h>

namespace kickos::esp32c6::reg::wdt
{
    // Common unlock key (MWDT + RWDT + SWD).
    constexpr uint32_t WKEY = 0x50D83AA1u;

    // MWDT: offsets into TIMG0_BASE / TIMG1_BASE.
    constexpr uintptr_t TIMG_WDTCONFIG0 = 0x48u; // EN=bit31, FLASHBOOT_MOD_EN=bit14
    constexpr uintptr_t TIMG_WDTWPROTECT = 0x64u;
    constexpr uint32_t TIMG_WDT_EN = 1u << 31;
    constexpr uint32_t TIMG_WDT_FLASHBOOT = 1u << 14;

    // RWDT + super-watchdog (SWD): offsets into RTC_WDT_BASE.
    constexpr uintptr_t RTC_WDT_CONFIG0 = 0x00u; // EN=bit31, FLASHBOOT_MOD_EN=bit12
    constexpr uintptr_t RTC_WDT_WPROTECT = 0x18u;
    constexpr uint32_t RTC_WDT_EN = 1u << 31;
    constexpr uint32_t RTC_WDT_FLASHBOOT = 1u << 12;
    constexpr uintptr_t RTC_SWD_CONFIG = 0x1Cu;   // SWD_DISABLE=bit30
    constexpr uintptr_t RTC_SWD_WPROTECT = 0x20u; // SWD's own write-protect key
    constexpr uint32_t RTC_SWD_DISABLE = 1u << 30;
}

#endif
