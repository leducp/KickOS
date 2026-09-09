// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/ramown.h>

#if KICKOS_HAVE_MPU

#include <kickos/arch/arch.h>
#include <kickos/task.h> // task_handle: the owner's identity, index AND generation

namespace kickos
{
    namespace
    {
        struct RamBlock
        {
            uintptr_t base = 0;
            // 0 => this slot describes no address. Every member's default is zero so the
            // table stays in .bss, and a free slot spans nothing AND names no task.
            uint32_t size = 0;
            kos_task_t owner = KOS_TASK_NONE;
        };

        RamBlock g_blocks[KICKOS_RAM_OWNER_SLOTS];

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
        // The slot is found BEFORE the arena is spent. The allocator never takes a block
        // back, so recording after allocating would drain the arena one lost block per call
        // once the table filled.
        RamBlock* slot = nullptr;
        for (RamBlock& b : g_blocks)
        {
            if (b.size == 0)
            {
                slot = &b;
                break;
            }
        }
        if (slot == nullptr)
        {
            return nullptr;
        }
        void* const p = arch_ram_alloc(size);
        if (p == nullptr)
        {
            return nullptr;
        }
        slot->base = reinterpret_cast<uintptr_t>(p);
        // What the allocator ACTUALLY reserved, which is the request rounded to a
        // describable region; recording the request would refuse a self-grant of the same
        // block at the extent the descriptor covers.
        slot->size = static_cast<uint32_t>(rsz);
        slot->owner = tag;
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
        for (RamBlock const& b : g_blocks)
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
}

#endif // KICKOS_HAVE_MPU
