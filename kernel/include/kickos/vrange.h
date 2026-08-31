// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The virtual ranges one address space names: what the syscall entry validates a user
// pointer against. Validation data, never enforcement: the page tables are what a
// wrong access faults on.
//
// Reserved and granted are two different claims: allocation reserves a page-aligned
// range and maps nothing, and the self-grant maps it. Only a granted range admits a pointer,
// so a caller cannot pass the kernel a buffer it has reserved but never made reachable.
//
// It is also the space's map record, which is what teardown reads: a space frees what it maps
// and the borrower unmaps first, so an entry carries whether these frames are the
// space's own or another space's.
//
// The granule is a parameter and no figure appears here.

#ifndef KICKOS_VRANGE_H
#define KICKOS_VRANGE_H

#include <kickos/arch/arch.h> // ARCH_MAP_*, which the width proofs below are over
#include <kickos/config/system.h>
#include <kickos/extent.h>

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

    enum : uint8_t
    {
        // The frames under this range belong to another space. Teardown unmaps them and
        // frees none: handing them to the frame pool would free an address it never owned.
        VR_BORROWED = 1u << 0,
        // The process image seeded into every space, rather than a range the app reserved.
        // A caller may name its own reservations and not these.
        VR_IMAGE = 1u << 1,
        // Placed by aspace_cap_map, which holds one reference on the frame RUN it maps.
        // VR_BORROWED alone does NOT identify such a range: the image and every handoff
        // carry it too. WHICH run is asked of arch_aspace_frame_at rather than stored here,
        // a field costing eight bytes in every range of every domain.
        VR_FRAMECAP = 1u << 2
    };

    // The most pages one range may name, which reserve() refuses above. This ceiling is what
    // makes the width of VirtualRange::pages a bound.
    constexpr size_t VR_MAX_PAGES = 0xFFFFFFFFu;

    struct VirtualRange
    {
        uintptr_t base = 0;
        uint32_t pages = 0;
        // The ARCH_MAP_* word the mapping carries, in the map editor's own vocabulary: no
        // translation sits between what was granted and what was installed.
        uint8_t rights = 0;
        // enum arch_map_memtype, narrowed. Two live mappings of one block must agree on it.
        uint8_t memtype = 0;
        uint8_t flags = 0; // VR_*
        VirtualState state = VirtualState::Free;
    };

    // Each of the three narrow fields holds the widest value its setter can be handed, so a
    // value the list would otherwise refuse cannot be stored as a smaller accepted one.
    // Narrowing a field or raising a ceiling past the other trips the matching line here.
    static_assert(VR_MAX_PAGES == static_cast<size_t>(static_cast<uint32_t>(VR_MAX_PAGES)),
                  "the reservation ceiling must round-trip through VirtualRange::pages");
    static_assert(static_cast<uint32_t>(ARCH_MAP_R | ARCH_MAP_W | ARCH_MAP_X)
                      == static_cast<uint32_t>(
                          static_cast<uint8_t>(ARCH_MAP_R | ARCH_MAP_W | ARCH_MAP_X)),
                  "every ARCH_MAP_* right must round-trip through VirtualRange::rights");
    static_assert(static_cast<unsigned>(ARCH_MAP_DEVICE)
                      == static_cast<unsigned>(static_cast<uint8_t>(ARCH_MAP_DEVICE)),
                  "every arch_map_memtype must round-trip through VirtualRange::memtype");

    class VirtualRanges
    {
    public:
        // `granule` must be a power of two; anything else leaves the list refusing every
        // call.
        bool init(size_t granule);

        // A page-aligned range no live entry overlaps, of at most VR_MAX_PAGES pages.
        // Reserving maps nothing.
        bool reserve(uintptr_t base, size_t pages, uint8_t flags = 0);

        // The range whose base is EXACTLY `base`, or null. Distinct from find(), which is
        // containment: a revoke must name a whole range and not a byte inside one.
        VirtualRange const* at_base(uintptr_t base) const;

        // Turn a reservation into a granted range. Exact: the pair must name a reservation
        // this space made. A rights word carrying a bit the entry cannot hold is refused,
        // never truncated.
        // No default memtype: 0 is Normal, and a caller omitting it over a non-cacheable leaf
        // records an agreement that is not the one installed.
        bool grant(uintptr_t base, size_t pages, uint32_t rights, uint8_t memtype);

        // Drop the entry starting at `base`, whatever its state.
        bool release(uintptr_t base);

        // The entry-path question: does [addr, addr + len) lie inside one granted range that
        // carries every right in `rights`? A range spanning two entries is refused even when
        // both are granted.
        bool covers(uintptr_t addr, size_t len, uint32_t rights) const;

        // Whether [base, base + pages * granule) meets any live entry. No precondition on the
        // extent: an extent whose byte count wraps answers true, so a caller that has not
        // checked its own arithmetic is refused rather than admitted on a wrapped end.
        bool overlaps(uintptr_t base, size_t pages) const;

        // The one live entry [addr, addr + len) lies inside, or null.
        VirtualRange const* find(uintptr_t addr, size_t len) const;

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
