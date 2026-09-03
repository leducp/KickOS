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
        // carry it too. WHICH run is the `run` field below.
        VR_FRAMECAP = 1u << 2,
        // An unprivileged thread's stack and the unmapped guard page under it, recorded so
        // that reserve() refuses anything landing on either. NEITHER RELEASE ARM OF THE
        // TEARDOWN WALK MAY TOUCH IT: its frames are ustack_free's.
        VR_USTACK = 1u << 3
    };

    // What the KERNEL placed rather than the caller: the process image, and every thread
    // stack with its guard. THE ONE FLAG LIST a caller-controlled admission path filters on,
    // so a further kernel-placed kind is added here and inherited everywhere.
    constexpr uint8_t VR_KERNEL_PLACED = static_cast<uint8_t>(VR_IMAGE | VR_USTACK);

    // The most pages one range may name, which reserve() refuses above. This ceiling is what
    // makes the width of VirtualRange::pages a bound.
    constexpr size_t VR_MAX_PAGES = 0xFFFFFFFFu;

    // This range names no frame run. ZERO, and the field holds the slot PLUS ONE, so the
    // whole record's default is all-zero and the domain array stays in .bss. A negative or
    // all-ones sentinel would make it dynamically initialised and move it into .data, which
    // is flash on every board that has any.
    constexpr uint32_t VR_RUN_NONE = 0u;

    struct VirtualRange
    {
        uintptr_t base = 0;
        uint32_t pages = 0;
        // The frame RUN a VR_FRAMECAP range names, as a SLOT INDEX PLUS ONE and not a handle:
        // a handle spends half its word on a generation.
        uint32_t run = VR_RUN_NONE;
        // The ARCH_MAP_* word the mapping carries, in the map editor's own vocabulary: no
        // translation sits between what was granted and what was installed.
        uint8_t rights = 0;
        // enum arch_map_memtype, narrowed. Two live mappings of one block must agree on it.
        uint8_t memtype = 0;
        uint8_t flags = 0; // VR_*
        VirtualState state = VirtualState::Free;
    };

    // Whether a CALLER may name this range in a syscall argument. Total: a null entry is an
    // address no live range describes, which is nobody's reservation either, so every
    // admission path can ask this one question and no site keeps a flag list of its own.
    inline bool vr_caller_nameable(VirtualRange const* e)
    {
        return e != nullptr and (e->flags & VR_KERNEL_PLACED) == 0u;
    }

    // THE GUARD'S SUBJECT CAN GROW AND THE FIELD CANNOT: raising the pool's Kconfig ceiling
    // past what this field can name must be a build error and never a truncated slot index
    // naming somebody else's run. The stored value is the slot PLUS ONE, hence the strict
    // inequality against the field's own width.
    static_assert(static_cast<unsigned long long>(KICKOS_MAX_FRAME_RUNS) + 1ull
                      < 0x100000000ull,
                  "a frame-run slot index plus one must fit VirtualRange::run, so the pool "
                  "cannot hold more runs than that field can name");

    // Each of the narrow fields holds the widest value its setter can be handed, so a
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

    // Slots the process IMAGE spends: its text and its data extents.
    constexpr size_t VR_IMAGE_SLOTS = 2u;
    // Slots left for the app's own mappings once the image and every thread stack are paid
    // for. A STRUCTURAL margin and NOT one app's demand: the self-grant arm asks the space
    // how many are left and takes one more, so it needs a success and a refusal, and the
    // capability arms hold two mappings at once. What the fleet's own selftest needs is much
    // larger and is the Kconfig DEFAULT's job, not this floor's; baking it in here would
    // force every translating board to provision for an app it does not run.
    constexpr size_t VR_APP_HEADROOM = 4u;

#if KICKOS_HAVE_ASPACE
    // A THREAD'S STACK TAKES A SLOT, so this budget scales with the thread count and the two
    // figures may not be configured independently. A task's siblings share its space, so the
    // worst case is every thread slot's stack in ONE list.
    //
    // VR_APP_HEADROOM may not fall to zero: the self-grant arm reads the free-slot count LIVE
    // and takes one more, so at zero free slots it can seat nothing and has nothing to report.
    static_assert(KICKOS_ASPACE_RANGES
                      >= VR_IMAGE_SLOTS + KICKOS_THREAD_SLOTS + VR_APP_HEADROOM,
                  "KICKOS_ASPACE_RANGES is below 2 + (KICKOS_MAX_THREADS + 1) + 4: the image "
                  "spends two slots, every live thread's stack spends one and returns it at "
                  "thread exit, and the app needs the rest. Raise KICKOS_ASPACE_RANGES or "
                  "lower KICKOS_MAX_THREADS");
#endif

    class VirtualRanges
    {
    public:
        // `granule` must be a power of two; anything else leaves the list refusing every
        // call.
        bool init(size_t granule);

        // A page-aligned range no live entry overlaps, of at most VR_MAX_PAGES pages.
        // Reserving maps nothing.
        //
        // `run` is the frame-run slot PLUS ONE that a VR_FRAMECAP range names, set HERE
        // rather than at grant: the reference it records was taken before this call, so a
        // reserve that succeeds and a grant that fails still leaves the run named by the
        // entry the unwind releases.
        bool reserve(uintptr_t base, size_t pages, uint8_t flags = 0,
                     uint32_t run = VR_RUN_NONE);

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
