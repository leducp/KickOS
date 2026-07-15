// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Generational slot pool: a fixed array of N slots, each with a generation counter.
// Handles pack (gen << kIndexBits) | index; free() bumps the slot's generation so a
// stale handle (naming a since-recycled slot) fails to resolve -- the ABA guard.
//
// Liveness here is EXTRINSIC: the pool owns a used[] bit per slot. The thread pool,
// whose liveness is INTRINSIC (a slot is free iff its TCB.state == EXITED, maintained
// by the scheduler as the single source of truth), migrates onto a liveness-policy
// variant later (see the M2 handle-table roadmap) -- do NOT force a redundant used[]
// bit onto it. Zero runtime cost: monomorphized per (T, N).
//
// Not internally locked -- the caller serializes (IrqLock today; the one place to add
// the SMP kernel lock later).

#ifndef KICKOS_SLOTPOOL_H
#define KICKOS_SLOTPOOL_H

#include <stdint.h>

namespace kickos
{
    template <class T, int N>
    class SlotPool
    {
        static constexpr int kIndexBits = 8; // handle low bits; the generation takes the rest
        static_assert(N <= (1 << kIndexBits), "SlotPool: N exceeds the handle index field");

    public:
        // Claim a free slot; returns its index, or -1 if the pool is full. The slot's
        // T is left as-is for the caller to initialize.
        [[nodiscard]] int alloc()
        {
            for (int i = 0; i < N; i++)
            {
                if (not used_[i])
                {
                    used_[i] = true;
                    return i;
                }
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
            int const index = handle & ((1 << kIndexBits) - 1);
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
            int const index = handle & ((1 << kIndexBits) - 1);
            // Full high bits, not truncated to 16: a handle carrying junk above the generation
            // field (bits set beyond what handle_for ever produces) must fail to resolve, not alias.
            uint32_t const gen = static_cast<uint32_t>(handle) >> kIndexBits;
            if (index >= N or not used_[index] or static_cast<uint32_t>(gen_[index]) != gen)
            {
                return nullptr;
            }
            return &slots_[index];
        }

        // The slot at a known-live index (for the caller to initialize after alloc()).
        T* at(int index) { return &slots_[index]; }

        // The opaque handle for a live slot index, carrying its current generation.
        int handle_for(int index) const
        {
            return static_cast<int>((static_cast<uint32_t>(gen_[index]) << kIndexBits) |
                                    static_cast<uint32_t>(index));
        }

    private:
        T slots_[N];
        bool used_[N] = {};    // all slots start free
        uint16_t gen_[N] = {}; // generations start at 0
    };
}

#endif
