// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The virtual ranges one address space names: what the syscall entry validates a user
// pointer against (docs/design-m6-mmu.md section 3.3). VALIDATION DATA, never enforcement:
// the page tables are what a wrong access faults on, and walking them on every syscall
// argument would put a multi-level traversal on the entry path to buy an answer the kernel
// already holds.
//
// Reserved and granted are two different claims, per F10: allocation reserves a page-aligned
// range and maps nothing, and the self-grant maps it. Only a GRANTED range admits a pointer,
// so a caller cannot pass the kernel a buffer it has reserved but never made reachable.
//
// The granule is a parameter and no figure appears here (F7).

#ifndef KICKOS_VRANGE_H
#define KICKOS_VRANGE_H

#include <kickos/config/system.h>

#include <stddef.h>
#include <stdint.h>

namespace kickos
{
    enum class VirtualState : uint8_t
    {
        Free = 0,
        Reserved = 1,
        Granted = 2
    };

    struct VirtualRange
    {
        uintptr_t base = 0;
        size_t pages = 0;
        // The ARCH_MAP_* word the mapping carries. The same vocabulary as the map editor's,
        // so no translation sits between what was granted and what was installed.
        uint32_t rights = 0;
        VirtualState state = VirtualState::Free;
    };

    class VirtualRanges
    {
    public:
        // `granule` must be a power of two; anything else leaves the list refusing every
        // call, since a list keyed on a granule it cannot check would admit a partial page.
        bool init(size_t granule);

        // A page-aligned range no live entry overlaps. Reserving maps nothing.
        bool reserve(uintptr_t base, size_t pages);

        // Turn a reservation into a granted range. EXACT: the pair must name a reservation
        // this space made, which is what stops one task naming a range another reserved.
        bool grant(uintptr_t base, size_t pages, uint32_t rights);

        // Drop the entry starting at `base`, whatever its state.
        bool release(uintptr_t base);

        // The entry-path question: does [addr, addr + len) lie inside ONE granted range that
        // carries every right in `rights`? A range spanning two entries is refused even when
        // both are granted, matching what the region check answers today.
        bool covers(uintptr_t addr, size_t len, uint32_t rights) const;

        bool overlaps(uintptr_t base, size_t pages) const;

        // Live entries, and the slot count they are spread over. `at` walks slots, not live
        // entries, so a free slot answers null rather than shifting its neighbours down.
        size_t count() const;
        static size_t capacity() { return KICKOS_ASPACE_RANGES; }
        VirtualRange const* at(size_t i) const;

    private:
        VirtualRange ranges_[KICKOS_ASPACE_RANGES];
        size_t granule_ = 0;
    };
}

#endif
