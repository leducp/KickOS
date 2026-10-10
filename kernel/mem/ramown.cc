// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/ramown.h>

#if KICKOS_HAVE_MPU

#include <kickos/ampshare.h>
#include <kickos/arch/arch.h>
#include <kickos/instance_local.h>
#include <kickos/task.h> // task_handle: the owner's identity, index AND generation

namespace kickos
{
    namespace
    {
        struct RamBlock
        {
            uintptr_t base = 0;
            // 0: a free slot. Every member's default is zero so the table stays in .bss, and a
            // free slot spans nothing and names no task.
            uint32_t size = 0;
            kos_task_t owner = KOS_TASK_NONE;
#if KICKOS_ARCH_ARENA_DCACHE
            bool sync_owed = false;
#endif
        };

        constinit InstanceLocal<RamBlock[KICKOS_RAM_OWNER_SLOTS]> g_owners = {};

        RamBlock (&blocks())[KICKOS_RAM_OWNER_SLOTS]
        {
            return g_owners.get();
        }

        RamBlock* free_block()
        {
            for (RamBlock& b : blocks())
            {
                if (b.size == 0)
                {
                    return &b;
                }
            }
            return nullptr;
        }

        static_assert(KICKOS_RAM_OWNER_SLOTS > 0,
                      "KICKOS_RAM_OWNER_SLOTS is 0: every kos_ram_alloc on this board would "
                      "answer NULL, since a block whose owner cannot be recorded is refused");
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
        // The slot is found before the arena is spent: the allocator never takes a block back,
        // so recording after allocating would drain the arena one lost block per call once the
        // table filled.
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
        *slot = RamBlock{reinterpret_cast<uintptr_t>(p), static_cast<uint32_t>(rsz), tag};
        return p;
    }

    bool ram_owner_nameable(Task const* owner, uintptr_t base, size_t size)
    {
        kos_task_t const tag = task_handle(owner);
        if (tag == KOS_TASK_NONE or size == 0)
        {
            return false;
        }
        size_t const rsz = arch_ram_region_size(size);
        if (rsz == 0)
        {
            return false;
        }
        uintptr_t const last = base + rsz - 1u;
        if (last < base)
        {
            return false; // the committed window wraps
        }
        for (RamBlock const& b : blocks())
        {
            if (b.size == 0 or b.owner != tag)
            {
                continue;
            }
            if (base >= b.base and last <= b.base + b.size - 1u)
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
        RamBlock* const b = free_block();
        if (b == nullptr)
        {
            return false;
        }
        *b = RamBlock{base, static_cast<uint32_t>(size), tag};
        return true;
    }
#endif
}

#endif // KICKOS_HAVE_MPU

#if not KICKOS_HAVE_ASPACE

#include <kickos/kruntime.h> // kmemset

namespace kickos
{
    void ram_block_clear(void* block, size_t extent)
    {
        kmemset(block, 0, extent);
#if KICKOS_ARCH_ARENA_DCACHE
        arch_dcache_invalidate(block, extent);
#endif
    }
}

#endif
