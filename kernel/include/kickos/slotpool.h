// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Generational slot pool: a fixed array of N slots, each with a generation counter.
// Handles pack (gen << INDEX_BITS) | index and spend all 32 bits, so a handle is NOT a
// signed quantity: alloc() signals a full pool with -1, which is an INDEX and never a
// handle. free() bumps the slot's generation so a stale handle (naming a since-recycled
// slot) fails to resolve: the ABA guard.
//
// The guard's strength is the WRAP DISTANCE. gen_ is uint16_t, so a stale handle naming
// slot i resolves again after 65536 frees OF THAT SLOT. That is why alloc() is NEXT-FIT:
// first-fit always returns the lowest free index, driving one slot's counter around its
// whole cycle while the others sit at 0.
//
// Liveness here is EXTRINSIC, a used[] bit per slot. The thread pool does not use this
// template; its liveness is intrinsic in TCB.state == EXITED.
//
// NOT internally locked: the caller serializes (IrqLock today).

#ifndef KICKOS_SLOTPOOL_H
#define KICKOS_SLOTPOOL_H

#include <stdint.h>

namespace kickos
{
    template <class T, int N>
    class SlotPool
    {
        // The uint16_t generation takes the other 16, so the handle spends the whole word. A
        // fully aged handle has bit 31 set and is NEGATIVE as an int, so neither free() nor
        // resolve() may test its sign.
        static constexpr int INDEX_BITS = 16;
        static constexpr uint32_t INDEX_MASK = (1u << INDEX_BITS) - 1u;
        // The one index value the pool never seats, so no handle a live slot can mint carries
        // an all-ones index. That keeps `-1`, and every other malformed word whose low half
        // is all ones, unresolvable rather than aliasing the top slot once its generation has
        // aged far enough.
        static_assert(N < (1 << INDEX_BITS), "SlotPool: N would seat the reserved all-ones index");
        // cursor_ stores an index, so widening INDEX_BITS past the cursor type would
        // truncate it and alias two slots onto one resume point.
        static_assert(N - 1 <= UINT16_MAX, "SlotPool: cursor_ too narrow for N");

        // The index after `index`, wrapped into [0, N).
        static int next_of(int index)
        {
            if (index + 1 >= N)
            {
                return 0;
            }
            return index + 1;
        }

    public:
        // Claim a free slot; returns its index, or -1 if the pool is full. The slot's
        // T is left as-is for the caller to initialize.
        //
        // The scan starts at cursor_ and walks the whole ring, so it visits every slot
        // exactly once and finds a free one whenever one exists. free() deliberately does
        // NOT rewind cursor_: pointing it back at the just-released slot is exactly the
        // first-fit concentration this avoids.
        [[nodiscard]] int alloc()
        {
            int index = cursor_;
            for (int step = 0; step < N; step++)
            {
                if (not used_[index])
                {
                    used_[index] = true;
                    cursor_ = static_cast<uint16_t>(next_of(index));
                    return index;
                }
                index = next_of(index);
            }
            return -1;
        }

        // Release the slot a handle names: bump its generation so outstanding handles
        // to it stop resolving, then mark it free. Self-guards the index (a safety-
        // critical primitive must not corrupt an adjacent slot on a malformed handle,
        // even though callers resolve() first today).
        //
        // NO SIGN TEST. An aged handle has bit 31 set, so `handle < 0` would silently refuse
        // to release live slots after 32768 recycles of one of them; the reserved all-ones
        // index is what keeps a `-1` out of range instead.
        void free(int handle)
        {
            int const index = static_cast<int>(static_cast<uint32_t>(handle) & INDEX_MASK);
            if (index >= N)
            {
                return;
            }
            gen_[index]++;
            used_[index] = false;
        }

        // Validate + resolve a handle to its slot, or nullptr if out-of-range, freed,
        // or stale (generation mismatch). No sign test, for free()'s reason.
        T* resolve(int handle)
        {
            uint32_t const u = static_cast<uint32_t>(handle);
            int const index = static_cast<int>(u & INDEX_MASK);
            uint32_t const gen = u >> INDEX_BITS;
            if (index >= N or not used_[index] or static_cast<uint32_t>(gen_[index]) != gen)
            {
                return nullptr;
            }
            return &slots_[index];
        }

        // The slot at a known-live index (for the caller to initialize after alloc()).
        T* at(int index) { return &slots_[index]; }

        // For a sweep keyed on OBJECT state rather than on a handle. A freed slot keeps its
        // last contents, so at() on one hands back stale fields: live() is what makes such
        // a sweep legal, not optional decoration.
        static constexpr int capacity() { return N; }
        bool live(int index) const { return used_[index]; }

        // Slot index of an object this pool handed out, for a caller holding the object but
        // not its handle; -1 if `p` is not one of our slot bases. Compares addresses as
        // integers, because subtracting pointers that may not point into slots_ is UB.
        // sizeof(T) is rarely a power of two, so a core with no divide instruction calls a
        // libgcc helper here; keep it off any per-message path.
        int index_of(T const* p) const
        {
            uintptr_t const base = reinterpret_cast<uintptr_t>(&slots_[0]);
            uintptr_t const q = reinterpret_cast<uintptr_t>(p);
            if (q < base)
            {
                return -1;
            }
            uintptr_t const off = q - base;
            if (off >= sizeof(slots_))
            {
                return -1;
            }
            if (off % sizeof(T) != 0)
            {
                return -1; // interior pointer, not a slot base
            }
            return static_cast<int>(off / sizeof(T));
        }

        // The opaque handle for a live slot index, carrying its current generation.
        int handle_for(int index) const
        {
            return static_cast<int>((static_cast<uint32_t>(gen_[index]) << INDEX_BITS) |
                                    static_cast<uint32_t>(index));
        }

    private:
        T slots_[N];
        bool used_[N] = {};    // all slots start free
        uint16_t cursor_ = 0;  // next-fit resume point; an index, always in [0, N)
        uint16_t gen_[N] = {}; // generations start at 0
    };
}

#endif
