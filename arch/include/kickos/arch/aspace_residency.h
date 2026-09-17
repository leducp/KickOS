// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Track each root's ASID and the cores that may retain its translations.
// Store metadata outside the root frame, which contains hardware descriptors.
// Residency persists after switching away. Untracked roots use ASID 0 and
// require maintenance on every core; the backend must handle ASID 0 safely.

#ifndef KICKOS_ARCH_ASPACE_RESIDENCY_H
#define KICKOS_ARCH_ASPACE_RESIDENCY_H

#include <stddef.h>
#include <stdint.h>

#if defined(__has_include) && __has_include(<kickos/board_config.h>)
#include <kickos/board_config.h>
#endif

#ifndef KICKOS_MAX_DOMAINS
#error "aspace_residency.h sizes its rows from KICKOS_MAX_DOMAINS and may not guess at a bound"
#endif

namespace kickos::aspace
{
    template <unsigned CORES>
    class Residency
    {
      public:
        static constexpr uint32_t EVERY_CORE = (1u << CORES) - 1u;

        // One row per domain slot. Hardware capacity may further limit the ASIDs.
        static constexpr size_t ROWS = KICKOS_MAX_DOMAINS;
        static constexpr uint32_t MAX_IDENTIFIER = ROWS;

        static_assert(CORES > 0 and CORES < 32, "the residency mask is one bit per core in a word");
        static_assert(ROWS > 0, "a record with no row answers its fallback to every question");

        // Reset residency after the old root has been invalidated on every core.
        // Reopening a row retains its ID. Capacity zero disables ID allocation.
        void open(uint64_t root, uint32_t capacity = 0)
        {
            size_t at = index_of(root);
            if (at == NONE)
            {
                if (live_ == ROWS)
                {
                    return;
                }
                at = live_;
                live_++;
                rows_[at].root = root;
                rows_[at].id = 0;
            }
            rows_[at].cores = 0;
            if (rows_[at].id == 0)
            {
                rows_[at].id = take_identifier(capacity);
            }
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
            uint32_t const bit = 1u << core;
            bool const had = (rows_[at].cores & bit) != 0;
            rows_[at].cores |= bit;
            return had;
        }

        uint32_t cores(uint64_t root) const
        {
            size_t const at = index_of(root);
            if (at == NONE)
            {
                return EVERY_CORE;
            }
            return rows_[at].cores;
        }

        // Return zero for untracked roots or exhausted ASIDs.
        uint32_t identifier(uint64_t root) const
        {
            size_t const at = index_of(root);
            if (at == NONE)
            {
                return 0;
            }
            return rows_[at].id;
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
            if (rows_[at].id != 0)
            {
                uint32_t const bit = rows_[at].id - 1u;
                taken_[bit / 32u] &= ~(1u << (bit % 32u));
            }
            live_--;
            rows_[at] = rows_[live_];
        }

      private:
        static constexpr size_t NONE = ROWS + 1;
        static constexpr size_t WORDS = (ROWS + 31) / 32;

        struct Row
        {
            uint64_t root;
            uint32_t cores;
            uint32_t id;
        };

        // Keep live rows contiguous so searches stop at the live count.
        size_t index_of(uint64_t root) const
        {
            for (size_t i = 0; i < live_; i++)
            {
                if (rows_[i].root == root)
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
            if (bound > MAX_IDENTIFIER)
            {
                bound = MAX_IDENTIFIER;
            }
            for (uint32_t bit = 0; bit < bound; bit++)
            {
                if ((taken_[bit / 32u] & (1u << (bit % 32u))) == 0)
                {
                    taken_[bit / 32u] |= 1u << (bit % 32u);
                    return bit + 1u;
                }
            }
            return 0;
        }

        Row rows_[ROWS] = {};
        size_t live_ = 0;
        // Bit i: identifier i + 1 is held by a live row.
        uint32_t taken_[WORDS] = {};
    };
}

#endif
