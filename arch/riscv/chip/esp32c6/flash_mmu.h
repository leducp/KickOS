// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The cache MMU entries that map the external memory window to flash, 64 KiB pages. The window
// is TRM v1.2 5.3.3.1; the entry layout is from ESP-IDF's soc/esp32c6 ext_mem_defs.h.

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_FLASH_MMU_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_FLASH_MMU_H

#include <stdint.h>

namespace kickos::esp32c6::flash_mmu
{
    constexpr uint32_t PAGE = 0x10000u;
    constexpr uint32_t WINDOW_BASE = 0x42000000u;
    constexpr uint32_t ENTRIES = 256u;
    // Entry bits 0 to 8 name the flash page, bit 9 makes the entry valid; 0 maps nothing.
    constexpr uint32_t VALID = 1u << 9;
    constexpr uint32_t INVALID = 0u;
    // 16 MiB of flash at 64 KiB a page.
    constexpr uint32_t PHYS_PAGES = 256u;

    // The entry mapping `va`, or ENTRIES where va is outside the window.
    constexpr uint32_t entry_of(uint32_t va)
    {
        if (va < WINDOW_BASE)
        {
            return ENTRIES;
        }
        uint32_t const page = (va - WINDOW_BASE) / PAGE;
        if (page >= ENTRIES)
        {
            return ENTRIES;
        }
        return page;
    }

    // The entry naming the flash page at physical `phys`, or INVALID where no entry can name it.
    constexpr uint32_t entry_value(uint32_t phys)
    {
        uint32_t const page = phys / PAGE;
        if (page >= PHYS_PAGES)
        {
            return INVALID;
        }
        return VALID | page;
    }

    // Pages from `start` to `end`, both on a page.
    constexpr uint32_t pages(uint32_t start, uint32_t end)
    {
        return (end - start) / PAGE;
    }

    // `write(entry, value)` for each page of [start, end), the page at `start + i * PAGE` naming
    // flash `load + i * PAGE`. Writes nothing and answers false when any page lies outside the
    // window or names flash no entry reaches.
    template <typename Write>
    bool map_text(uint32_t start, uint32_t end, uint32_t load, Write&& write)
    {
        uint32_t const n = pages(start, end);
        for (uint32_t i = 0; i < n; i++)
        {
            if (entry_of(start + i * PAGE) == ENTRIES or entry_value(load + i * PAGE) == INVALID)
            {
                return false;
            }
        }
        for (uint32_t i = 0; i < n; i++)
        {
            write(entry_of(start + i * PAGE), entry_value(load + i * PAGE));
        }
        return true;
    }
}

#endif
