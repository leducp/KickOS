// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The per-address-space virtual range list. kickos/vrange.h carries the contract.

#include <kickos/vrange.h>

namespace kickos
{
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
        uintptr_t end = 0;
        if (not extent_end(base, pages, granule_, &end))
        {
            return true; // a window that wraps overlaps everything this list could name
        }
        for (size_t i = 0; i < KICKOS_ASPACE_RANGES; i++)
        {
            if (ranges_[i].state == VirtualState::Free)
            {
                continue;
            }
            if (base < end_of(ranges_[i]) and ranges_[i].base < end)
            {
                return true;
            }
        }
        return false;
    }

    bool VirtualRanges::reserve(uintptr_t base, size_t pages, uint8_t flags, uint32_t run,
                                uint16_t holder, uint16_t donor)
    {
        if (granule_ == 0 or pages == 0 or (base & (granule_ - 1u)) != 0)
        {
            return false;
        }
        if (pages > VR_MAX_PAGES)
        {
            return false; // the entry cannot hold the count, and truncating it would admit it
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
                ranges_[i].pages = static_cast<uint32_t>(pages);
                ranges_[i].run = run;
                ranges_[i].rights = 0;
                ranges_[i].memtype = 0;
                ranges_[i].flags = flags;
                ranges_[i].state = VirtualState::Reserved;
                ranges_[i].holder = holder;
                ranges_[i].donor = donor;
                return true;
            }
        }
        return false;
    }

    uintptr_t VirtualRanges::place(uintptr_t area, size_t area_pages, size_t pages) const
    {
        uintptr_t area_end = 0;
        uintptr_t end = 0;
        if (granule_ == 0 or pages == 0 or pages > area_pages or (area & (granule_ - 1u)) != 0
            or not extent_end(area, area_pages, granule_, &area_end))
        {
            return 0;
        }
        uintptr_t base = area;
        // Each step moves past one live entry, so the walk ends after as many steps as there
        // are entries.
        while (extent_end(base, pages, granule_, &end) and end <= area_end)
        {
            VirtualRange const* hit = nullptr;
            for (size_t i = 0; i < KICKOS_ASPACE_RANGES and hit == nullptr; i++)
            {
                VirtualRange const& r = ranges_[i];
                if (r.state != VirtualState::Free and base < end_of(r) and r.base < end)
                {
                    hit = &r;
                }
            }
            if (hit == nullptr)
            {
                return base;
            }
            base = end_of(*hit);
        }
        return 0;
    }

    bool VirtualRanges::grant(uintptr_t base, size_t pages, uint32_t rights, uint8_t memtype)
    {
        if (granule_ == 0 or rights == 0)
        {
            return false;
        }
        if (rights != static_cast<uint32_t>(static_cast<uint8_t>(rights)))
        {
            return false; // a right the entry cannot hold, refused rather than dropped
        }
        size_t const i = index_at(base);
        if (i == KICKOS_ASPACE_RANGES or ranges_[i].pages != pages)
        {
            return false;
        }
        ranges_[i].rights = static_cast<uint8_t>(rights);
        ranges_[i].memtype = memtype;
        ranges_[i].state = VirtualState::Granted;
        return true;
    }

    bool VirtualRanges::set_sync_owed(uintptr_t base, bool owed)
    {
        size_t const i = index_at(base);
        if (i == KICKOS_ASPACE_RANGES)
        {
            return false;
        }
        uint8_t f = static_cast<uint8_t>(ranges_[i].flags & ~VR_SYNC_OWED);
        if (owed)
        {
            f = static_cast<uint8_t>(f | VR_SYNC_OWED);
        }
        ranges_[i].flags = f;
        return true;
    }

    bool VirtualRanges::release(uintptr_t base)
    {
        size_t const i = index_at(base);
        if (i == KICKOS_ASPACE_RANGES)
        {
            return false;
        }
        ranges_[i] = VirtualRange{};
        return true;
    }

    VirtualRange const* VirtualRanges::at_base(uintptr_t base) const
    {
        return at(index_at(base));
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
            if (addr >= ranges_[i].base and end <= end_of(ranges_[i]))
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
            if (addr >= ranges_[i].base and end <= end_of(ranges_[i])
                and (ranges_[i].rights & rights) == rights)
            {
                return true;
            }
        }
        return false;
    }

    size_t VirtualRanges::index_at(uintptr_t base) const
    {
        size_t i = 0;
        while (i < KICKOS_ASPACE_RANGES
               and (ranges_[i].state == VirtualState::Free or ranges_[i].base != base))
        {
            i++;
        }
        return i;
    }

    uintptr_t VirtualRanges::end_of(VirtualRange const& r) const
    {
        return r.base + static_cast<uintptr_t>(r.pages) * granule_;
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
