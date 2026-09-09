// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Which task reserved which arena block, on the backend that describes REGIONS. The bump
// allocator (arch/common/arch_ram_common.cc) hands back a base and keeps no owner, so without
// this record every in-arena, descriptor-encodable range is admissible to every caller: a
// spawn could name a sibling task's data as its child's domain.
//
// THE TRANSLATING TWIN OF THIS RECORD IS THE SPACE'S OWN VirtualRanges (vrange.h), where a
// reservation is an address in one task's namespace and the range list already answers
// ownership. This file is the region-side backend of that one question, and nothing else
// answers it on either side.
//
// TOTAL over an address no block describes: that is nobody's reservation, so every admission
// path can ask this and no site keeps a list of its own. The kernel's OWN arena blocks (the
// boot stacks, the thread pool's default stacks, the sim's guard page) reach arch_ram_alloc
// directly and are recorded nowhere, which is what stops a caller naming one.
//
// NOTHING FREES A RECORD, exactly as nothing gives an arena block back. A dead task's blocks
// stay recorded against a task handle whose generation has moved, so a recycled task slot
// inherits nothing and the blocks are nameable by nobody.

#ifndef KICKOS_RAMOWN_H
#define KICKOS_RAMOWN_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/arch/arch.h> // arch_ram_alloc, for the unenforced arm below
#include <kickos/config.h>

namespace kickos
{
    struct Task; // kickos/task.h: the owner, one per group of threads

#if KICKOS_HAVE_MPU
    // The arena allocation AND its ownership record, which cannot be taken apart: a block
    // handed out with no owner is nameable by nobody and lost for the life of the image.
    // Null when EITHER the arena or the table is exhausted, and the arm that fails spends
    // nothing; kos_ram_alloc has no route out for an errno, so the two are indistinguishable.
    //
    // Caller holds IrqLock: the bump pointer and this table are read-modify-written together.
    void* ram_owner_alloc(Task const* owner, size_t size);

    // Whether [base, base + size) AS IT WILL BE COMMITTED lies inside one block `owner`
    // reserved. THE COMMITTED EXTENT AND NOT THE NAMED ONE: every descriptor is sized with
    // arch_ram_region_size, and on a base+limit backend that rounding can carry an interior
    // base past the end of the block it started in.
    //
    // A null owner, a task no handle names, size 0 and a window that wraps all answer false.
    bool ram_owner_nameable(Task const* owner, uintptr_t base, size_t size);
#else
    // No region descriptors: nothing is enforced here, so nothing is owned either. A
    // translating backend takes this arm too, its record being the range list.
    //
    // The stubs fold each admission branch away, but NOT the argument that reaches them: a
    // site whose owner costs a sched::current() carries its own #if, the six
    // KICKOS_KERNEL_STACKS=0 presets being measured to the byte on that path.
    inline void* ram_owner_alloc(Task const*, size_t size) { return arch_ram_alloc(size); }
    inline bool ram_owner_nameable(Task const*, uintptr_t, size_t) { return true; }
#endif
}

#endif
