/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 */

#ifndef KICKOS_ARCH_PE_SECTIONS_H
#define KICKOS_ARCH_PE_SECTIONS_H

#include <stddef.h>
#include <stdint.h>

namespace kickos::x86_64
{
    constexpr uint32_t pe_scn_mem_discardable = 0x02000000u;
    constexpr uint32_t pe_scn_mem_write = 0x80000000u;
    constexpr size_t pe_section_header_bytes = 40;

    // Is [ptr, ptr + len) inside ONE section of the PE32+ image loaded at `base`, `size` bytes
    // long, whose `count` section headers start at `sections`, and writable where asked?
    inline bool pe_range_mapped(uintptr_t base, size_t size, uint8_t const* sections,
                                unsigned count, uintptr_t ptr, size_t len, bool need_write)
    {
        if (len == 0)
        {
            return true;
        }
        if (sections == nullptr)
        {
            return false;
        }
        uintptr_t const end = ptr + len;
        if (end < ptr)
        {
            return false;
        }
        if (ptr < base or end > base + size)
        {
            return false;
        }
        for (unsigned i = 0; i < count; i++)
        {
            uint8_t const* const s = sections + i * pe_section_header_bytes;
            uint32_t const characteristics = *reinterpret_cast<uint32_t const*>(s + 36);
            // The loader is free not to map a discardable section at all.
            if ((characteristics & pe_scn_mem_discardable) != 0)
            {
                continue;
            }
            uint32_t const virtual_size = *reinterpret_cast<uint32_t const*>(s + 8);
            uint32_t const rva = *reinterpret_cast<uint32_t const*>(s + 12);
            uint32_t const raw_size = *reinterpret_cast<uint32_t const*>(s + 16);
            uint32_t span = virtual_size;
            if (raw_size > span)
            {
                span = raw_size;
            }
            uintptr_t const lo = base + rva;
            if (ptr < lo or end > lo + span)
            {
                continue;
            }
            if (need_write and (characteristics & pe_scn_mem_write) == 0)
            {
                return false;
            }
            return true;
        }
        return false;
    }
}

#endif
