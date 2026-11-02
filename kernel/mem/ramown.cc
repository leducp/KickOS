// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/ramown.h>

#if not KICKOS_HAVE_ASPACE

#include <kickos/ampshare.h>
#include <kickos/arch/arch.h>
#include <kickos/budget.h>
#include <kickos/kruntime.h> // kmemset
#include <kickos/pools.h>
#include <kickos/task.h> // task_handle: the owner's identity, index AND generation

namespace kickos
{
    namespace
    {
        PoolSpan<RamBlock> blocks()
        {
            return pool_span(kickos_kernel_pools.ram_owners, kickos_kernel_pools.ram_owner_count);
        }

        // The records below it are taken, the rest free: nothing frees a record.
        constinit InstanceLocal<uint16_t> g_fresh = {0};

        // The first free record, or nullptr when every one is taken. O(1).
        RamBlock* free_block()
        {
            PoolSpan<RamBlock> const all = blocks();
            if (g_fresh.get() >= all.size())
            {
                return nullptr;
            }
            return &all[g_fresh.get()];
        }

        // Whether `owner` may take one more block of `rsz` bytes.
        bool admit(Task const* owner, size_t rsz)
        {
            return task_admit(owner, BudgetKind::RAM_BLOCKS, 1)
                   and task_admit(owner, BudgetKind::RAM_BYTES, static_cast<uint32_t>(rsz));
        }

        // `b`, the record free_block answered, is `owner`'s, and its record pays for it from here
        // on.
        void record(Task const* owner, RamBlock* b, uintptr_t base, uint32_t size)
        {
            uint16_t* const head = task_ram_head(owner);
            g_fresh.get()++;
            *b = RamBlock{base, size, task_handle(owner)};
            b->next = *head;
            *head = static_cast<uint16_t>(b - blocks().begin() + 1);
        }
    }

    void* ram_owner_alloc(Task const* owner, size_t size)
    {
        kos_task_t const tag = task_handle(owner);
        if (tag == KOS_TASK_NONE or size == 0)
        {
            return nullptr;
        }
        size_t const rsz = arch_ram_region_size(size);
        if (rsz != static_cast<size_t>(static_cast<uint32_t>(rsz)))
        {
            return nullptr; // an extent RamBlock::size cannot hold, never a truncated one
        }
        // The budget and the slot before the arena is spent: the allocator never takes a block
        // back, so recording after allocating would drain the arena one lost block per call once
        // the table filled.
        if (not admit(owner, rsz))
        {
            return nullptr;
        }
        RamBlock* const slot = free_block();
        if (slot == nullptr)
        {
            return nullptr;
        }
        void* const p = arch_ram_alloc(size);
        if (p == nullptr)
        {
            return nullptr;
        }
        // The size the allocator reserved, the request rounded to a describable region:
        // recording the request would refuse a self-grant of the block at the extent the
        // descriptor covers.
        record(owner, slot, reinterpret_cast<uintptr_t>(p), static_cast<uint32_t>(rsz));
        return p;
    }

    bool ram_owner_nameable(Task const* owner, uintptr_t base, size_t size)
    {
        kos_task_t const tag = task_handle(owner);
        if (tag == KOS_TASK_NONE or size == 0)
        {
            return false;
        }
        size_t const rsz = ram_grant_extent(size);
        if (rsz == 0)
        {
            return false;
        }
        uintptr_t const last = base + rsz - 1u;
        if (last < base)
        {
            return false; // the committed window wraps
        }
        // The record's chain holds every incarnation's blocks; only this one's are nameable.
        // O(the record's blocks).
        RamBlock const* const all = blocks().begin();
        for (uint16_t at = *task_ram_head(owner); at != 0; at = all[at - 1].next)
        {
            RamBlock const& b = all[at - 1];
            if (b.owner == tag and base >= b.base and last <= b.base + b.size - 1u)
            {
                return true;
            }
        }
        return false;
    }

#if KICKOS_ARCH_ARENA_DCACHE
    bool ram_owner_sync_owed(uintptr_t base, size_t size)
    {
        for (RamBlock const& b : blocks())
        {
            if (b.size != 0 and b.sync_owed and base < b.base + b.size and b.base < base + size)
            {
                return true;
            }
        }
        return false;
    }

    void ram_owner_set_sync_owed(uintptr_t base, size_t size, bool owed)
    {
        for (RamBlock& b : blocks())
        {
            if (b.size != 0 and base < b.base + b.size and b.base < base + size)
            {
                b.sync_owed = owed;
            }
        }
    }
#endif

#if KICKOS_AMP_SHARE
    bool ram_owner_seat(Task const* owner, uintptr_t base, size_t size)
    {
        kos_task_t const tag = task_handle(owner);
        if (tag == KOS_TASK_NONE or size == 0 or arch_ram_region_size(size) != size
            or size != static_cast<size_t>(static_cast<uint32_t>(size))
            or not arch_ram_region_admissible(base, size))
        {
            return false;
        }
        if (not admit(owner, size))
        {
            return false;
        }
        RamBlock* const b = free_block();
        if (b == nullptr)
        {
            return false;
        }
        record(owner, b, base, static_cast<uint32_t>(size));
        return true;
    }
#endif

    void ram_block_clear(void* block, size_t extent)
    {
        kmemset(block, 0, extent);
#if KICKOS_ARCH_ARENA_DCACHE
        arch_dcache_invalidate(block, extent);
#endif
    }
}

#endif
