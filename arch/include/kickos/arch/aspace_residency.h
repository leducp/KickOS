// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Track each root's ASID, the cores that may retain its translations and the table pages it holds.
// Store metadata outside the root frame, which contains hardware descriptors.
// Residency persists after switching away. Untracked roots use ASID 0 and
// require maintenance on every core; the backend must handle ASID 0 safely.

#ifndef KICKOS_ARCH_ASPACE_RESIDENCY_H
#define KICKOS_ARCH_ASPACE_RESIDENCY_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/pools.h>

#if KICKOS_HAVE_ASPACE
namespace kickos::aspace
{
    // A tracked root: its identifier, the cores that may hold its translations and its table
    // pages, the root's own included.
    struct ResidencyRow
    {
        uint64_t root = 0;
        uint32_t cores = 0;
        uint32_t id = 0;
        uint32_t tables = 0;
    };

    // One row per domain slot, in the pools' storage, once per machine. Hardware capacity may
    // further limit the ASIDs.
    template <unsigned CORES>
    class Residency
    {
      public:
        static constexpr uint32_t EVERY_CORE = (1u << CORES) - 1u;

        static_assert(CORES > 0 and CORES < 32, "the residency mask is one bit per core in a word");

        // Reset residency after the old root has been invalidated on every core, the root
        // holding `tables` table pages. Reopening a row retains its ID. Capacity zero disables ID
        // allocation. False where every row is live, which leaves the root untracked.
        bool open(uint64_t root, uint32_t tables, uint32_t capacity = 0)
        {
            ResidencyRow* const rows = rows_();
            size_t at = index_of(root);
            if (at == NONE)
            {
                if (live_ == row_count())
                {
                    return false;
                }
                at = live_;
                live_++;
                rows[at].root = root;
                rows[at].id = 0;
            }
            rows[at].cores = 0;
            rows[at].tables = tables;
            if (rows[at].id == 0)
            {
                rows[at].id = take_identifier(capacity);
            }
            return true;
        }

        // Record residency when installing a root.
        void note(uint64_t root, uint32_t core)
        {
            (void)note_and_was_resident(root, core);
        }

        // Record this core and return whether it was already resident.
        // For untracked roots, return false to require publication on every entry.
        // cores() instead returns every core so invalidation cannot omit a holder.
        bool note_and_was_resident(uint64_t root, uint32_t core)
        {
            size_t const at = index_of(root);
            if (at == NONE)
            {
                return false;
            }
            ResidencyRow* const rows = rows_();
            uint32_t const bit = 1u << core;
            bool const had = (rows[at].cores & bit) != 0;
            rows[at].cores |= bit;
            return had;
        }

        uint32_t cores(uint64_t root) const
        {
            size_t const at = index_of(root);
            if (at == NONE)
            {
                return EVERY_CORE;
            }
            return rows_()[at].cores;
        }

        // The table page count of `root`, which every table linked or given back moves. An
        // untracked root's is a sink no reader trusts.
        uint32_t* tables(uint64_t root)
        {
            size_t const at = index_of(root);
            if (at == NONE)
            {
                return &untracked_tables_;
            }
            return &rows_()[at].tables;
        }

        // Return zero for untracked roots or exhausted ASIDs.
        uint32_t identifier(uint64_t root) const
        {
            size_t const at = index_of(root);
            if (at == NONE)
            {
                return 0;
            }
            return rows_()[at].id;
        }

        // Call after invalidation on every resident core and before reusing the root
        // or ASID.
        void close(uint64_t root)
        {
            size_t const at = index_of(root);
            if (at == NONE)
            {
                return;
            }
            ResidencyRow* const rows = rows_();
            uint32_t* const taken = taken_();
            if (rows[at].id != 0)
            {
                uint32_t const bit = rows[at].id - 1u;
                taken[bit / 32u] &= ~(1u << (bit % 32u));
            }
            live_--;
            rows[at] = rows[live_];
        }

      private:
        static constexpr size_t NONE = SIZE_MAX;

        static ResidencyRow* rows_()
        {
            return kickos_kernel_pools.residency_rows;
        }
        static uint32_t* taken_()
        {
            return kickos_kernel_pools.residency_taken;
        }
        // A row per domain slot, and as many identifiers.
        static size_t row_count()
        {
            return static_cast<size_t>(kickos_kernel_pools.domain_count);
        }

        // Keep live rows contiguous so searches stop at the live count.
        size_t index_of(uint64_t root) const
        {
            ResidencyRow const* const rows = rows_();
            for (size_t i = 0; i < live_; i++)
            {
                if (rows[i].root == root)
                {
                    return i;
                }
            }
            return NONE;
        }

        // Return the lowest free nonzero ID within the hardware and row limits.
        uint32_t take_identifier(uint32_t capacity)
        {
            uint32_t bound = capacity;
            uint32_t const rows = static_cast<uint32_t>(row_count());
            if (bound > rows)
            {
                bound = rows;
            }
            uint32_t* const taken = taken_();
            for (uint32_t bit = 0; bit < bound; bit++)
            {
                if ((taken[bit / 32u] & (1u << (bit % 32u))) == 0)
                {
                    taken[bit / 32u] |= 1u << (bit % 32u);
                    return bit + 1u;
                }
            }
            return 0;
        }

        size_t live_ = 0;
        uint32_t untracked_tables_ = 0;
    };
}
#endif

#endif
