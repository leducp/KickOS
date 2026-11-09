// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The cache MMU entries that map VAddr1 to flash for PID 0 and 1, the only PIDs this port runs
// (ESP32 TRM v5.8 4.3.2.2, p.83-85).

#ifndef KICKOS_ARCH_XTENSA_CHIP_ESP32_FLASH_MMU_H
#define KICKOS_ARCH_XTENSA_CHIP_ESP32_FLASH_MMU_H

#include <stdint.h>

namespace kickos::esp32::flash_mmu
{
    constexpr uint32_t PAGE = 0x10000u;
    // Table 4.3-9: VAddr1, 64 pages from 0x4000_0000.
    constexpr uint32_t VADDR1_BASE = 0x40000000u;
    constexpr uint32_t VADDR1_PAGES = 64u;
    // Tables 4.3-10 and 4.3-11: VAddr1's first entry for PID 0/1, APP_CPU's 2048 above PRO_CPU's.
    constexpr uint32_t VADDR1_PID01_FIRST = 64u;
    constexpr uint32_t APP_FIRST = 2048u;
    constexpr uint32_t ENTRIES = 4096u;
    // Bit 8 set: the entry maps nothing.
    constexpr uint32_t INVALID = 1u << 8;
    // Physical pages one entry can name: bits 0 to 7.
    constexpr uint32_t PHYS_PAGES = 256u;

    // The entry mapping `va` for `core` (0 PRO, 1 APP), or ENTRIES where va is outside VAddr1.
    constexpr uint32_t vaddr1_entry(uint32_t core, uint32_t va)
    {
        if (va < VADDR1_BASE or core > 1u)
        {
            return ENTRIES;
        }
        uint32_t const page = (va - VADDR1_BASE) / PAGE;
        if (page >= VADDR1_PAGES)
        {
            return ENTRIES;
        }
        return core * APP_FIRST + VADDR1_PID01_FIRST + page;
    }

    // The entry naming the flash page at physical `phys`, valid; INVALID where no entry can name it.
    constexpr uint32_t entry_value(uint32_t phys)
    {
        uint32_t const page = phys / PAGE;
        if (page >= PHYS_PAGES)
        {
            return INVALID;
        }
        return page;
    }

    // Pages from `start` to `end`, both on a page.
    constexpr uint32_t pages(uint32_t start, uint32_t end)
    {
        return (end - start) / PAGE;
    }

    // `write(entry, value)` for each page of [start, end) on each of `cores`, the page at
    // `start + i * PAGE` naming flash `load + i * PAGE`. Writes nothing and answers false when
    // any page lies outside VAddr1.
    template <typename Write>
    bool map_text(uint32_t cores, uint32_t start, uint32_t end, uint32_t load, Write&& write)
    {
        uint32_t const n = pages(start, end);
        for (uint32_t i = 0; i < n; i++)
        {
            for (uint32_t core = 0; core < cores; core++)
            {
                if (vaddr1_entry(core, start + i * PAGE) == ENTRIES)
                {
                    return false;
                }
            }
        }
        for (uint32_t i = 0; i < n; i++)
        {
            for (uint32_t core = 0; core < cores; core++)
            {
                write(vaddr1_entry(core, start + i * PAGE), entry_value(load + i * PAGE));
            }
        }
        return true;
    }
}

#endif
