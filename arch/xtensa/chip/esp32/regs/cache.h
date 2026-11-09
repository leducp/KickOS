// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32 flash cache control: the two CPUs' cache registers in DPORT (TRM v5.8 Registers 12.10 to
// 12.14, p.249-253), the cache MMU table (Table 3.3-6, p.72) and SPI0's cache request gate.

#ifndef KICKOS_ARCH_XTENSA_CHIP_ESP32_REGS_CACHE_H
#define KICKOS_ARCH_XTENSA_CHIP_ESP32_REGS_CACHE_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::esp32::reg::cache
{
    constexpr uintptr_t PRO_CTRL = mmap::DPORT_BASE + 0x040u;  // Register 12.10
    constexpr uintptr_t PRO_CTRL1 = mmap::DPORT_BASE + 0x044u; // Register 12.11
    constexpr uintptr_t APP_CTRL = mmap::DPORT_BASE + 0x058u;  // Register 12.12
    constexpr uintptr_t APP_CTRL1 = mmap::DPORT_BASE + 0x05Cu; // Register 12.13
    constexpr uintptr_t MUX_MODE = mmap::DPORT_BASE + 0x07Cu;  // Register 12.14

    // x_CACHE_CTRL_REG. The flush DISCARDS what the cache holds (3.3.4, p.72).
    constexpr uint32_t ENABLE = 1u << 3;
    constexpr uint32_t FLUSH_ENA = 1u << 4;
    constexpr uint32_t FLUSH_DONE = 1u << 5;
    constexpr uint32_t SINGLE_IRAM_ENA = 1u << 10;
    constexpr uint32_t DRAM_SPLIT = 1u << 11;
    constexpr uint32_t DRAM_HL = 1u << 16;

    // x_CACHE_CTRL1_REG: a set MASK bit closes that bus to the cache. IRAM0 is VAddr1, the only
    // bus this port opens, so the 4.3.2.1 Note on invalid entries under several open
    // instruction buses never applies.
    constexpr uint32_t MASK_IRAM0 = 1u << 0;
    constexpr uint32_t MASK_ALL = 0x3Fu;
    constexpr uint32_t MMU_IA_CLR = 1u << 13;

    // Mode 0: POOL0 backs PRO_CPU, POOL1 APP_CPU, the only mode both caches run under at once
    // (Table 3.3-5, p.71).
    constexpr uint32_t MUX_MODE_SPLIT = 0u;

    constexpr uintptr_t MMU_TABLE = 0x3FF10000u;

    // SPI_CACHE_FCTRL_REG of SPI0, absent from the TRM's register list: the ROM's
    // Cache_Read_Enable sets it before the DPORT enable, and ESP-IDF names it
    // (components/soc/esp32/register/soc/spi_reg.h).
    constexpr uintptr_t SPI0_CACHE_FCTRL = 0x3FF43050u;
    constexpr uint32_t SPI0_CACHE_REQ_EN = 1u << 0;
}

#endif
