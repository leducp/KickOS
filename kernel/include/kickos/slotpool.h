// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Generational slot pool: an array of slots, each with a generation counter, in the pools'
// storage (<kickos/pools.h>).
// Handles pack (gen << HANDLE_INDEX_BITS) | index and spend all 32 bits, so a handle is NOT a
// signed quantity: alloc() signals a full pool with -1, which is an INDEX and never a
// handle. free() bumps the slot's generation so a stale handle (naming a since-recycled
// slot) fails to resolve: the ABA guard.
//
// The guard's strength is the WRAP DISTANCE. A handle carries the low 16 bits of the slot's
// 32-bit generation, so a stale handle naming slot i resolves again after 65536 frees OF THAT
// SLOT; a capability entry keeps the whole generation beside the handle and checks it against
// generations(). That is why the free list is a FIFO behind the never-used slots: a LIFO
// hands the slot just freed straight back, driving one slot's counter around its whole cycle
// while the others sit at 0.
//
// Liveness here is EXTRINSIC: a slot is live once claimed (below `fresh`) while its node is off
// the free list. The thread pool does not use this template; its liveness is intrinsic in
// TCB.state == EXITED.
//
// NOT internally locked: the caller serializes (IrqLock).

#ifndef KICKOS_SLOTPOOL_H
#define KICKOS_SLOTPOOL_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/budget.h>          // task_admit: every claim is charged
#include <kickos/config/pool_abi.h> // KICKOS_HANDLE_INDEX_BITS (generated)
#include <kickos/list.h>
#include <kickos/pools.h>           // SlotArrays

namespace kickos
{
    // The handle codec every generational pool shares: (gen << HANDLE_INDEX_BITS) | index. The
    // uint16_t generation takes the other 16 bits, so a fully aged handle has bit 31 set and is
    // NEGATIVE as an int: no decoder may test its sign.
    constexpr int HANDLE_INDEX_BITS = KICKOS_HANDLE_INDEX_BITS;
    constexpr uint32_t HANDLE_INDEX_MASK = (1u << HANDLE_INDEX_BITS) - 1u;
    static_assert(HANDLE_INDEX_BITS == 16,
                  "handle_gen takes the generation as the uint16_t above the index");

    constexpr uint32_t handle_pack(uint16_t gen, uint32_t index)
    {
        return (static_cast<uint32_t>(gen) << HANDLE_INDEX_BITS) | index;
    }

    constexpr uint32_t handle_index(uint32_t handle)
    {
        return handle & HANDLE_INDEX_MASK;
    }

    constexpr uint16_t handle_gen(uint32_t handle)
    {
        return static_cast<uint16_t>(handle >> HANDLE_INDEX_BITS);
    }

    // Index of `p` among the `count` slots from `base`, or -1 if `p` is not one of their element
    // bases, nullptr included. Compares addresses as integers: subtracting pointers that may not
    // point into the array is UB. sizeof(T) is rarely a power of two, so a core with no divide
    // instruction calls a libgcc helper here; keep it off any per-message path.
    template <class T>
    int slot_index_of(T const* base, int count, T const* p)
    {
        uintptr_t const lo = reinterpret_cast<uintptr_t>(base);
        uintptr_t const q = reinterpret_cast<uintptr_t>(p);
        size_t const bytes = static_cast<size_t>(count) * sizeof(T);
        if (p == nullptr or q < lo or q - lo >= bytes or (q - lo) % sizeof(T) != 0)
        {
            return -1;
        }
        return static_cast<int>((q - lo) / sizeof(T));
    }

    // What a claim answers when the charged task holds its budget of the pool's kind, beside -1
    // for a full pool. An index, never a handle.
    constexpr int SLOT_OVER_BUDGET = -2;

    // A pool's free list: slots at and above `fresh` were never claimed, and `free` holds the
    // released ones, the one freed longest ago first.
    struct SlotPoolState
    {
        uint16_t fresh = 0;
        List free;
    };

    // The bodies of SlotPool below, one for every pool: `a` is the pool's arrays as the pools
    // descriptor holds them.
    [[nodiscard]] int slot_claim(SlotPoolState& st, SlotArrays const* a, BudgetKind kind,
                                 Task const* charged);
    void slot_release(SlotPoolState& st, SlotArrays const* a, int handle);
    int slot_live_index(SlotPoolState const& st, SlotArrays const* a, int handle);

    // Store names the pool's storage (<kickos/pools.h>): Store::slots() answers its slots,
    // Store::arrays() the descriptor of its side arrays, of Store::count() entries each, and
    // Store::KIND the budget a claim is charged to. Every slot count is below the all-ones
    // index, so no handle a live slot can mint carries one: that keeps `-1`, and every other
    // malformed word whose low half is all ones, unresolvable rather than aliasing the top slot
    // once its generation has aged far enough. A T the holdings count (cap.cc) carries a
    // uint16_t hold_mark.
    template <class T, class Store>
    class SlotPool
    {
    public:
        // Claim a free slot charged to `charged`'s budget of the pool's kind; returns its index,
        // SLOT_OVER_BUDGET when the budget refuses, or -1 if the pool is full. The slot's T is
        // left as-is for the caller to initialize. O(1) past the admission: a never-used slot
        // first, then the one freed longest ago.
        [[nodiscard]] int alloc(Task const* charged)
        {
            return slot_claim(state_, Store::arrays(), Store::KIND, charged);
        }

        // Release the slot a handle names: bump its generation so outstanding handles
        // to it stop resolving, then mark it free. Self-guards the index and the slot's
        // liveness: a malformed or stale handle must not corrupt an adjacent slot or link a
        // free one twice, even though callers resolve() first. O(1).
        //
        // NO SIGN TEST. An aged handle has bit 31 set, so `handle < 0` would silently refuse
        // to release live slots after 32768 recycles of one of them; the reserved all-ones
        // index is what keeps a `-1` out of range instead.
        void free(int handle)
        {
            slot_release(state_, Store::arrays(), handle);
        }

        // Validate + resolve a handle to its slot, or nullptr if out-of-range, freed,
        // or stale (generation mismatch).
        T* resolve(int handle)
        {
            int const index = live_index(handle);
            if (index < 0)
            {
                return nullptr;
            }
            return &Store::slots()[index];
        }

        // The validated slot INDEX a handle names, or -1 for out-of-range, freed or stale.
        // index_of(resolve(h)) recovers it by pointer subtraction and so spends a DIVIDE that
        // this never does. O(1).
        int live_index(int handle) const
        {
            return slot_live_index(state_, Store::arrays(), handle);
        }

        // Each slot's whole generation, count() of them, indexed by handle_index.
        uint32_t const* generations() const
        {
            return pool_local(Store::arrays()->gen);
        }

        // The object-side refcount of each slot, count() of them.
        uint8_t* refs() const
        {
            return pool_local(Store::arrays()->refs);
        }

        int count() const
        {
            return Store::count();
        }

        // The slot at `index`, or nullptr where `index` is outside [0, count). THE BOUND ALONE:
        // a freed slot still hands back its last occupant's fields. Takes an INDEX and never a
        // handle, so `at(alloc())` needs no test of its own.
        T* at(int index)
        {
            if (static_cast<uint32_t>(index) >= static_cast<uint32_t>(Store::count()))
            {
                return nullptr;
            }
            return &Store::slots()[index];
        }

        // The slot an index this pool answered names, alloc's or live_index's: no bound, the
        // index being one of the pool's already.
        T* slot(int index)
        {
            return &Store::slots()[index];
        }

        // For a caller holding the object but not its handle. nullptr answers -1, so
        // index_of(resolve(h)) is one refusal and not two.
        int index_of(T const* p) const
        {
            return slot_index_of<T>(Store::slots(), Store::count(), p);
        }

        // The opaque handle for a live slot index, carrying its current generation; -1 for an
        // index outside [0, count). That -1 needs no sentinel test at a consumer: its low half
        // is the reserved all-ones index, so resolve() and free() already answer nothing for it.
        int handle_for(int index) const
        {
            if (static_cast<uint32_t>(index) >= static_cast<uint32_t>(Store::count()))
            {
                return -1;
            }
            return static_cast<int>(handle_pack(static_cast<uint16_t>(generations()[index]),
                                                static_cast<uint32_t>(index)));
        }

    private:
        SlotPoolState state_;
    };
}

#endif
