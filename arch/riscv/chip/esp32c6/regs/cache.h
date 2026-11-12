// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 flash cache: the cache MMU in SPI0, the L1 cache's bus and sync controls, and SPI0's
// clock. The TRM v1.2 documents none of these but PCR_CACHE_CONF; the rest is from ESP-IDF's
// soc/esp32c6 register headers (spi_mem_reg.h, extmem_reg.h, pcr_reg.h) and the ROM's
// Cache_* routines (esp32c6_rev0_rom.elf).

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_CACHE_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_CACHE_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::esp32c6::reg::cache
{
    constexpr uintptr_t SPI0_BASE = mmap::SPI0_BASE;
    constexpr uintptr_t SPI0_CLOCK = SPI0_BASE + 0x014u;
    constexpr uintptr_t MMU_ITEM_CONTENT = SPI0_BASE + 0x37Cu;
    constexpr uintptr_t MMU_ITEM_INDEX = SPI0_BASE + 0x380u;
    constexpr uintptr_t MMU_POWER_CTRL = SPI0_BASE + 0x384u;

    // MMU_POWER_CTRL: the page size field, 0 for 64 KiB.
    constexpr uint32_t MMU_PAGE_SIZE_S = 3u; // [4:3]
    constexpr uint32_t MMU_PAGE_SIZE_MASK = 0x3u;
    constexpr uint32_t MMU_PAGE_SIZE_64K = 0u;

    // SPI0_CLOCK: SPI0's flash clock is MSPI_FAST_CLK / (N + 1), H = (N + 1) / 2 - 1, L = N.
    constexpr uint32_t CLKCNT_N_S = 16u;
    constexpr uint32_t CLKCNT_H_S = 8u;
    constexpr uint32_t CLKCNT_L_S = 0u;

    constexpr uintptr_t EXTMEM_BASE = mmap::EXTMEM_BASE;
    constexpr uintptr_t L1_CTRL = EXTMEM_BASE + 0x004u;
    constexpr uintptr_t L1_SYNC_CTRL = EXTMEM_BASE + 0x098u;
    constexpr uintptr_t L1_SYNC_ADDR = EXTMEM_BASE + 0x0A0u;
    constexpr uintptr_t L1_SYNC_SIZE = EXTMEM_BASE + 0x0A4u;
    constexpr uintptr_t L1_PRELOAD_CTRL = EXTMEM_BASE + 0x0D8u;
    constexpr uintptr_t L1_AUTOLOAD_CTRL = EXTMEM_BASE + 0x134u;

    // L1_CTRL: a set bit closes that bus to the cache.
    constexpr uint32_t SHUT_IBUS = 1u << 0;
    constexpr uint32_t SHUT_DBUS = 1u << 1;
    // L1_SYNC_CTRL: an invalidate over SYNC_ADDR 0 and SYNC_SIZE 0 is the whole cache.
    constexpr uint32_t INVALIDATE_ENA = 1u << 0;
    constexpr uint32_t SYNC_DONE = 1u << 4;
    constexpr uint32_t SYNC_SIZE_MASK = 0x00FFFFFFu;
    constexpr uint32_t PRELOAD_DONE = 1u << 1;
    constexpr uint32_t AUTOLOAD_ENA = 1u << 0;
    constexpr uint32_t AUTOLOAD_DONE = 1u << 1;

    constexpr uintptr_t PCR_MSPI_CLK_CONF = mmap::PCR_BASE + 0x01Cu;
    constexpr uintptr_t PCR_CACHE_CONF = mmap::PCR_BASE + 0x104u; // TRM Register 8.60
    constexpr uint32_t PCR_CACHE_CLK_EN = 1u << 0;
    // PCR_MSPI_CLK_CONF: MSPI_FAST_CLK is the 480 MHz SPLL / (HS + 1) with the PLL the SoC
    // root; the reset HS of 3 gives 120 MHz.
    constexpr uint32_t MSPI_FAST_HS_DIV_S = 8u; // [15:8]
    constexpr uint32_t MSPI_FAST_HS_DIV_MASK = 0xFFu;
    constexpr uint32_t MSPI_FAST_HS_DIV_80M = 5u;
}

#endif
