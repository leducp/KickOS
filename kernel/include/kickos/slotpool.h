// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Generational slot pool: a fixed array of N slots, each with a generation counter.
// Handles pack (gen << INDEX_BITS) | index; free() bumps the slot's generation so a
// stale handle (naming a since-recycled slot) fails to resolve: the ABA guard.
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
        static constexpr int INDEX_BITS = 8; // handle low bits; the generation takes the rest
        static_assert(N <= (1 << INDEX_BITS), "SlotPool: N exceeds the handle index field");
        // cursor_ stores an index, so widening INDEX_BITS past the cursor type would
        // truncate it and alias two slots onto one resume point.
        static_assert(N - 1 <= UINT8_MAX, "SlotPool: cursor_ too narrow for N");

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
                    cursor_ = static_cast<uint8_t>(next_of(index));
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
        void free(int handle)
        {
            if (handle < 0)
            {
                return;
            }
            int const index = handle & ((1 << INDEX_BITS) - 1);
            if (index >= N)
            {
                return;
            }
            gen_[index]++;
            used_[index] = false;
        }

        // Validate + resolve a handle to its slot, or nullptr if out-of-range, freed,
        // or stale (generation mismatch).
        T* resolve(int handle)
        {
            if (handle < 0)
            {
                return nullptr;
            }
            int const index = handle & ((1 << INDEX_BITS) - 1);
            // Full high bits, not truncated to 16: a handle carrying junk above the generation
            // field (bits set beyond what handle_for ever produces) must fail to resolve, not alias.
            uint32_t const gen = static_cast<uint32_t>(handle) >> INDEX_BITS;
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

        // The opaque handle for a live slot index, carrying its current generation.
        int handle_for(int index) const
        {
            return static_cast<int>((static_cast<uint32_t>(gen_[index]) << INDEX_BITS) |
                                    static_cast<uint32_t>(index));
        }

    private:
        T slots_[N];
        bool used_[N] = {};    // all slots start free
        uint8_t cursor_ = 0;   // next-fit resume point; an index, always in [0, N)
        uint16_t gen_[N] = {}; // generations start at 0
    };
}

#endif
