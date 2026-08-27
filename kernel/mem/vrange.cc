// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The per-address-space virtual range list. kickos/vrange.h carries the contract.

#include <kickos/vrange.h>

namespace kickos
{
    namespace
    {
        bool is_pow2(size_t v)
        {
            if (v == 0)
            {
                return false;
            }
            return (v & (v - 1u)) == 0;
        }
    }

    bool VirtualRanges::init(size_t granule)
    {
        for (size_t i = 0; i < KICKOS_ASPACE_RANGES; i++)
        {
            ranges_[i] = VirtualRange{};
        }
        granule_ = 0;
        if (not is_pow2(granule))
        {
            return false;
        }
        granule_ = granule;
        return true;
    }

    bool VirtualRanges::overlaps(uintptr_t base, size_t pages) const
    {
        if (granule_ == 0 or pages == 0)
        {
            return false;
        }
        uintptr_t const end = base + static_cast<uintptr_t>(pages) * granule_;
        for (size_t i = 0; i < KICKOS_ASPACE_RANGES; i++)
        {
            if (ranges_[i].state == VirtualState::Free)
            {
                continue;
            }
            uintptr_t const lo = ranges_[i].base;
            uintptr_t const hi = lo + static_cast<uintptr_t>(ranges_[i].pages) * granule_;
            if (base < hi and lo < end)
            {
                return true;
            }
        }
        return false;
    }

    bool VirtualRanges::reserve(uintptr_t base, size_t pages, uint8_t flags)
    {
        if (granule_ == 0 or pages == 0 or (base & (granule_ - 1u)) != 0)
        {
            return false;
        }
        uintptr_t const bytes = static_cast<uintptr_t>(pages) * granule_;
        if (bytes / granule_ != pages or base + bytes < base)
        {
            return false;
        }
        if (overlaps(base, pages))
        {
            return false;
        }
        for (size_t i = 0; i < KICKOS_ASPACE_RANGES; i++)
        {
            if (ranges_[i].state == VirtualState::Free)
            {
                ranges_[i].base = base;
                ranges_[i].pages = pages;
                ranges_[i].rights = 0;
                ranges_[i].memtype = 0;
                ranges_[i].flags = flags;
                ranges_[i].state = VirtualState::Reserved;
                return true;
            }
        }
        return false;
    }

    bool VirtualRanges::grant(uintptr_t base, size_t pages, uint32_t rights, uint8_t memtype)
    {
        if (granule_ == 0 or rights == 0)
        {
            return false;
        }
        for (size_t i = 0; i < KICKOS_ASPACE_RANGES; i++)
        {
            if (ranges_[i].state == VirtualState::Free or ranges_[i].base != base)
            {
                continue;
            }
            if (ranges_[i].pages != pages)
            {
                return false;
            }
            ranges_[i].rights = rights;
            ranges_[i].memtype = memtype;
            ranges_[i].state = VirtualState::Granted;
            return true;
        }
        return false;
    }

    bool VirtualRanges::release(uintptr_t base)
    {
        for (size_t i = 0; i < KICKOS_ASPACE_RANGES; i++)
        {
            if (ranges_[i].state != VirtualState::Free and ranges_[i].base == base)
            {
                ranges_[i] = VirtualRange{};
                return true;
            }
        }
        return false;
    }

    VirtualRange const* VirtualRanges::find(uintptr_t addr, size_t len) const
    {
        if (granule_ == 0 or len == 0)
        {
            return nullptr;
        }
        uintptr_t const end = addr + len;
        if (end < addr)
        {
            return nullptr;
        }
        for (size_t i = 0; i < KICKOS_ASPACE_RANGES; i++)
        {
            if (ranges_[i].state == VirtualState::Free)
            {
                continue;
            }
            uintptr_t const lo = ranges_[i].base;
            uintptr_t const hi = lo + static_cast<uintptr_t>(ranges_[i].pages) * granule_;
            if (addr >= lo and end <= hi)
            {
                return &ranges_[i];
            }
        }
        return nullptr;
    }

    bool VirtualRanges::covers(uintptr_t addr, size_t len, uint32_t rights) const
    {
        if (granule_ == 0 or len == 0)
        {
            return false;
        }
        uintptr_t const end = addr + len;
        if (end < addr)
        {
            return false;
        }
        for (size_t i = 0; i < KICKOS_ASPACE_RANGES; i++)
        {
            if (ranges_[i].state != VirtualState::Granted)
            {
                continue;
            }
            uintptr_t const lo = ranges_[i].base;
            uintptr_t const hi = lo + static_cast<uintptr_t>(ranges_[i].pages) * granule_;
            if (addr >= lo and end <= hi and (ranges_[i].rights & rights) == rights)
            {
                return true;
            }
        }
        return false;
    }

    size_t VirtualRanges::count() const
    {
        size_t n = 0;
        for (size_t i = 0; i < KICKOS_ASPACE_RANGES; i++)
        {
            if (ranges_[i].state != VirtualState::Free)
            {
                n++;
            }
        }
        return n;
    }

    VirtualRange const* VirtualRanges::at(size_t i) const
    {
        if (i >= KICKOS_ASPACE_RANGES or ranges_[i].state == VirtualState::Free)
        {
            return nullptr;
        }
        return &ranges_[i];
    }
}
