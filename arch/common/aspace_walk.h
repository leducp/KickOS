// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The page-table walk the translating backends share: map, unmap, the unwind of a failed map and
// the frees of destroy, over a table format each backend spells. A backend defines GRANULE_SHIFT,
// GRANULE, INDEX_BITS and PTES, then includes this once, at namespace scope, and defines the hooks
// declared below. Heights count from the leaf table, 0, up to the root, walk_top().

#ifndef KICKOS_ARCH_COMMON_ASPACE_WALK_H
#define KICKOS_ARCH_COMMON_ASPACE_WALK_H

#include <kickos/arch/arch.h>

#include <stddef.h>
#include <stdint.h>

extern "C"
{
    arch_phys_addr_t kickos_frame_alloc(void);
    void kickos_frame_free(arch_phys_addr_t frame);
}

namespace
{
    // One edit of one space, from its first store to its last maintenance.
    struct Edit
    {
        // Some core may hold translations of the space, the caller's included.
        bool resident;
        // The other cores that may, sampled before the first store.
        uint32_t peers;
        // A removed leaf owes the peers more than the translation maintenance.
        bool owed;
        // The first table taken out of the tree and not yet given back, or 0.
        arch_phys_addr_t detached;
    };

    int walk_top();
    uint64_t* table_at(arch_phys_addr_t pa);
    arch_phys_addr_t entry_pa(uint64_t desc);
    // A valid entry at `height` that names a table, not a larger leaf.
    bool entry_table(uint64_t desc, int height);
    // A root entry the space shares with the boot root.
    bool root_slot_kept(uint64_t const* root, size_t slot);
    uint64_t leaf_desc(uint64_t attrs, arch_phys_addr_t pa);
    // Stores the entry naming a zeroed table, ordered after the zeroing for the walker.
    void link_table(uint64_t* entry, arch_phys_addr_t table, Edit const& e);
    // Drops the translation of `va`, ordered after the store that changed it.
    void invalidate_page(uintptr_t va);
    bool removal_owes(uint64_t desc);
    // Publishes the edits and synchronises every core that may hold them. Where a table was
    // detached, no walk through it survives this call.
    void edits_done(Edit const& e);

    // Bit 0 is the valid bit of an A64, an Sv39 or Sv48, and an x86-64 entry alike.
    constexpr uint64_t ENTRY_VALID = 1u;
    static_assert(GRANULE == static_cast<size_t>(1) << GRANULE_SHIFT, "one granule size");
    static_assert(PTES * sizeof(uint64_t) == GRANULE, "a table is one granule of entries");
    static_assert(PTES == static_cast<size_t>(1) << INDEX_BITS, "an index spans one table");

    size_t index_at(uintptr_t va, int height)
    {
        unsigned const shift = GRANULE_SHIFT + static_cast<unsigned>(height) * INDEX_BITS;
        return static_cast<size_t>((va >> shift) & (PTES - 1));
    }

    uintptr_t span_at(int height)
    {
        unsigned const shift = GRANULE_SHIFT + static_cast<unsigned>(height) * INDEX_BITS;
        return static_cast<uintptr_t>(1) << shift;
    }

    void zero_table(uint64_t* table)
    {
        for (size_t i = 0; i < PTES; i++)
        {
            table[i] = 0;
        }
    }

    bool table_empty(uint64_t const* table)
    {
        for (size_t i = 0; i < PTES; i++)
        {
            if (table[i] != 0)
            {
                return false;
            }
        }
        return true;
    }

    void invalidate_page_if(uintptr_t va, bool resident)
    {
        if (not resident)
        {
            return;
        }
        invalidate_page(va);
    }

    // The granule leaf at `va`, or null. A larger leaf on the way sets *larger.
    uint64_t* walk_leaf(uint64_t* root, uintptr_t va, bool* larger)
    {
        uint64_t* table = root;
        for (int height = walk_top(); height > 0; height--)
        {
            uint64_t const desc = table[index_at(va, height)];
            if ((desc & ENTRY_VALID) == 0)
            {
                return nullptr;
            }
            if (not entry_table(desc, height))
            {
                *larger = true;
                return nullptr;
            }
            table = table_at(entry_pa(desc));
        }
        uint64_t* const entry = &table[index_at(va, 0)];
        if ((*entry & ENTRY_VALID) == 0)
        {
            return nullptr;
        }
        return entry;
    }

    uint64_t* leaf_entry(uint64_t* root, uintptr_t va)
    {
        bool larger = false;
        return walk_leaf(root, va, &larger);
    }

    // Frees every table under `table` and every output it names. Kept root slots stay.
    void free_subtree(uint64_t* table, int height, bool root)
    {
        for (size_t i = 0; i < PTES; i++)
        {
            uint64_t const desc = table[i];
            if ((desc & ENTRY_VALID) == 0)
            {
                continue;
            }
            if (root and root_slot_kept(table, i))
            {
                continue;
            }
            if (height > 0 and entry_table(desc, height))
            {
                free_subtree(table_at(entry_pa(desc)), height - 1, false);
            }
            // The allocator ignores outputs it does not own, such as device pages.
            kickos_frame_free(entry_pa(desc));
            table[i] = 0;
        }
    }

    enum arch_aspace_result map_into(uint64_t* table, int height, uintptr_t va, size_t pages,
                                     arch_phys_addr_t pa, uint64_t attrs, Edit& e)
    {
        while (pages != 0)
        {
            size_t const idx = index_at(va, height);
            if (height == 0)
            {
                if ((table[idx] & ENTRY_VALID) != 0)
                {
                    if (removal_owes(table[idx]))
                    {
                        e.owed = true;
                    }
                    // Break before make.
                    table[idx] = 0;
                    invalidate_page_if(va, e.resident);
                }
                table[idx] = leaf_desc(attrs, pa);
                invalidate_page_if(va, e.resident);
                va += GRANULE;
                pa += GRANULE;
                pages--;
                continue;
            }

            if ((table[idx] & ENTRY_VALID) == 0)
            {
                arch_phys_addr_t const frame = kickos_frame_alloc();
                if (frame == 0)
                {
                    return ARCH_ASPACE_ENOMEM;
                }
                zero_table(table_at(frame));
                link_table(&table[idx], frame, e);
            }
            uintptr_t const span = span_at(height);
            size_t here = static_cast<size_t>((((va + span) & ~(span - 1)) - va) / GRANULE);
            if (here > pages)
            {
                here = pages;
            }
            enum arch_aspace_result const rc =
                map_into(table_at(entry_pa(table[idx])), height - 1, va, here, pa, attrs, e);
            if (rc != ARCH_ASPACE_OK)
            {
                return rc;
            }
            va += static_cast<uintptr_t>(here) * GRANULE;
            pa += static_cast<arch_phys_addr_t>(here) * GRANULE;
            pages -= here;
        }
        return ARCH_ASPACE_OK;
    }

    // Takes every table [va, end) reaches under `table` and leaves empty out of the tree, onto
    // e.detached; returns whether `table` is left empty. A detached table keeps the next one in
    // its first entry, bit 0 clear, so a walk still holding it reads nothing there.
    bool detach_empty(uint64_t* table, int height, uintptr_t va, uintptr_t end, bool root,
                      Edit& e)
    {
        uintptr_t at = va;
        while (at < end)
        {
            size_t const idx = index_at(at, height);
            uintptr_t next = (at & ~(span_at(height) - 1)) + span_at(height);
            if (next > end)
            {
                next = end;
            }
            uint64_t const desc = table[idx];
            if (entry_table(desc, height) and not (root and root_slot_kept(table, idx)))
            {
                uint64_t* const child = table_at(entry_pa(desc));
                bool empty = false;
                if (height == 1)
                {
                    empty = table_empty(child);
                }
                else
                {
                    empty = detach_empty(child, height - 1, at, next, false, e);
                }
                if (empty)
                {
                    table[idx] = 0;
                    child[0] = e.detached;
                    e.detached = entry_pa(desc);
                }
            }
            at = next;
        }
        return table_empty(table);
    }

    // Clears the leaves of [va, va + pages) up to the first page without one, then gives back
    // the tables that leaves empty in the range.
    void remove_run(uint64_t* root, uintptr_t va, size_t pages, Edit& e)
    {
        for (size_t i = 0; i < pages; i++)
        {
            uintptr_t const at = va + static_cast<uintptr_t>(i) * GRANULE;
            uint64_t* const entry = leaf_entry(root, at);
            if (entry == nullptr)
            {
                break;
            }
            if (removal_owes(*entry))
            {
                e.owed = true;
            }
            *entry = 0;
            invalidate_page_if(at, e.resident);
        }
        (void)detach_empty(root, walk_top(), va, va + static_cast<uintptr_t>(pages) * GRANULE,
                           true, e);
        edits_done(e);
        while (e.detached != 0)
        {
            arch_phys_addr_t const table = e.detached;
            e.detached = table_at(table)[0];
            kickos_frame_free(table);
        }
    }

    // Refuses a range a larger leaf covers, and one partly mapped, before any edit: an unwind
    // removes only leaves this call installed. A wholly mapped range is a remap.
    enum arch_aspace_result walk_map(uint64_t* root, uintptr_t va, arch_phys_addr_t pa,
                                     size_t pages, uint64_t attrs, Edit e)
    {
        size_t mapped = 0;
        for (size_t i = 0; i < pages; i++)
        {
            bool larger = false;
            if (walk_leaf(root, va + static_cast<uintptr_t>(i) * GRANULE, &larger) != nullptr)
            {
                mapped++;
            }
            if (larger)
            {
                return ARCH_ASPACE_EINVAL;
            }
        }
        if (mapped != 0 and mapped != pages)
        {
            return ARCH_ASPACE_EINVAL;
        }
        enum arch_aspace_result const rc = map_into(root, walk_top(), va, pages, pa, attrs, e);
        if (rc != ARCH_ASPACE_OK)
        {
            remove_run(root, va, pages, e);
            return rc;
        }
        edits_done(e);
        return ARCH_ASPACE_OK;
    }

    enum arch_aspace_result walk_unmap(uint64_t* root, uintptr_t va, size_t pages, Edit e)
    {
        for (size_t i = 0; i < pages; i++)
        {
            if (leaf_entry(root, va + static_cast<uintptr_t>(i) * GRANULE) == nullptr)
            {
                return ARCH_ASPACE_EINVAL;
            }
        }
        remove_run(root, va, pages, e);
        return ARCH_ASPACE_OK;
    }
}

#endif
