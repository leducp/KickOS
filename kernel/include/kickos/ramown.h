// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Which task reserved which arena block, on every backend that does not translate. The bump
// allocator (arch/common/arch_ram_common.cc) keeps no owner, so without this record every
// in-arena range is admissible to every caller. The translating twin is the space's own
// VirtualRanges (vrange.h).
//
// Total over an address no block describes: that is nobody's reservation. The kernel's own
// arena blocks (boot stacks, the thread pool's default stacks, the sim's guard page) reach
// arch_ram_alloc directly and are recorded nowhere, which is what stops a caller naming one.
//
// Nothing frees a record, as nothing gives an arena block back. A dead task's blocks stay
// recorded against a task handle whose generation has moved, so they are nameable by nobody.

#ifndef KICKOS_RAMOWN_H
#define KICKOS_RAMOWN_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/arch/arch.h>
#include <kickos/config.h>

#include <kickos/sys/abi.h> // kos_task_t

namespace kickos
{
    struct Task; // kickos/task.h: the owner, one per group of threads

    // The extent a grant of `size` bytes of RAM commits, and whether one at `base` of that
    // extent can be committed: one region descriptor's rounding and shape where the region unit
    // is programmed, the request as it stands where none is.
    inline size_t ram_grant_extent(size_t size)
    {
#if KICKOS_HAVE_MPU
        return arch_ram_region_size(size);
#else
        return size;
#endif
    }
    inline bool ram_grant_shaped(uintptr_t base, size_t extent)
    {
#if KICKOS_HAVE_MPU
        return arch_ram_region_admissible(base, extent);
#else
        (void)base;
        return extent != 0;
#endif
    }

#if not KICKOS_HAVE_ASPACE
    // One reservation, in the pools' storage (<kickos/pools.h>).
    struct RamBlock
    {
        uintptr_t base = 0;
        // 0: a free slot. Every member's default is zero so the table stays in .bss, and a
        // free slot spans nothing and names no task.
        uint32_t size = 0;
        kos_task_t owner = KOS_TASK_NONE;
        // The next record the same task record pays for, its index plus one (task_ram_head).
        uint16_t next = 0;
#if KICKOS_ARCH_ARENA_DCACHE
        bool sync_owed = false;
#endif
    };

    // The arena allocation and its ownership record, which cannot be taken apart: a block
    // handed out with no owner is nameable by nobody and lost for the life of the image.
    // Null when either the arena or the table is exhausted, and the arm that fails spends
    // nothing; kos_ram_alloc has no route out for an errno, so the two are indistinguishable.
    //
    // Caller holds IrqLock: the bump pointer and this table are read-modify-written together.
    void* ram_owner_alloc(Task const* owner, size_t size);

    // Whether [base, base + size) as it will be committed (ram_grant_extent) lies inside one
    // block `owner` reserved: on a base+limit backend the rounding can carry an interior base
    // past the end of the block it started in.
    //
    // A null owner, a task no handle names, size 0 and a window that wraps all answer false.
    bool ram_owner_nameable(Task const* owner, uintptr_t base, size_t size);

    // Record [base, base + size), a block outside the arena, as `owner`'s. False where the
    // table is full, `owner` no task, or the extent not one region describes exactly.
    bool ram_owner_seat(Task const* owner, uintptr_t base, size_t size);

#if KICKOS_ARCH_ARENA_DCACHE
    // Whether a block [base, base + size) meets has been held by a non-cacheable region since
    // its last sync, and the record of it for every block it meets. Caller holds IrqLock.
    bool ram_owner_sync_owed(uintptr_t base, size_t size);
    void ram_owner_set_sync_owed(uintptr_t base, size_t size, bool owed);
#endif

    // Zero a block kos_ram_alloc hands out over `extent`, the span a region grants. Where the
    // arch puts a data cache over the arena the zeroes are cleaned to memory and the lines
    // dropped, so a mapping of the block that bypasses the cache reads the zeroes and no later
    // eviction writes a stale line over what that mapping stored.
    void ram_block_clear(void* block, size_t extent);
#endif
}

#endif
