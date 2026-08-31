// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Rule 7: the grant path REFUSES a region that overlaps a kernel-reserved block. The
// reserved set is the arch's owns-for-life peripherals: the timebase, the IRQ
// controller, every access-permission controller (the MPU/PMP twin and any bus-side
// gate) and the clock/reset gates. Each enforcing chip declares it via
// arch_reserved_blocks (arch.h).
//
// The geometry and Rule 7 checks compile only where memory protection is LIVE
// (KICKOS_MEMORY_ENFORCED), which a translating backend sets while carrying no region
// descriptors at all; the stubs below keep the call sites #if-free.

#ifndef KICKOS_GRANT_H
#define KICKOS_GRANT_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/arch/arch.h> // ARCH_MPU_NOCACHE, arch_mpu_nocache_support
#include <kickos/config.h>    // the configuration umbrella

// Fill target for arch_reserved_blocks: must stay >= the most blocks any chip declares
// (imxrt1062 = 7 today). Every body truncates silently once it hits this cap.
#define KICKOS_MAX_RESERVED 8u

namespace kickos
{
    // Overlap of [a_base,a_last] with [b_base,b_last]. Adjacency (a_last+1 == b_base) is
    // NOT overlap: a grant may sit flush against a reserved block (the mk64f PIT CH2
    // case). Callers pass non-wrapping ranges (last >= base).
    inline bool grant_ranges_overlap(uintptr_t a_base, uintptr_t a_last,
                                     uintptr_t b_base, uintptr_t b_last)
    {
        return a_base <= b_last and b_base <= a_last;
    }

    // A commit backend silently DROPS a region whose memory type it cannot encode, so an
    // unencodable ARCH_MPU_NOCACHE must be refused here. Live in BOTH postures.
    //
    // THE QUESTION GOES TO WHICHEVER FAMILY COMMITS THE MAPPING. A translating board
    // seats no region descriptor and answers ARCH_MPU_NOCACHE_REFUSED to the region query
    // whatever its page tables can encode, so asking that one there refuses every memory
    // type the map editor honours.
    inline bool grant_nocache_admissible(uint32_t attr)
    {
        if ((attr & ARCH_MPU_NOCACHE) == 0)
        {
            return true;
        }
#if KICKOS_HAVE_ASPACE
        return arch_aspace_memtype_support(ARCH_MAP_NOCACHE);
#else
        return arch_mpu_nocache_support() != ARCH_MPU_NOCACHE_REFUSED;
#endif
    }

    // Whether the mapping itself CARRIES the memory type, so a block already reachable
    // cacheably is not already reachable as the caller asked. A page table always carries
    // it; a region set carries it only where the descriptor has the field.
    inline bool grant_memtype_programmed(void)
    {
#if KICKOS_HAVE_ASPACE
        return true;
#else
        return arch_mpu_nocache_support() == ARCH_MPU_NOCACHE_PROGRAMMED;
#endif
    }

#if KICKOS_MEMORY_ENFORCED
    // True iff [base, base+size) touches any reserved block, or on a bit-band chip the
    // alias image of a reserved block lying in the aliasable 1 MB peripheral region.
    // size 0 touches nothing (shape checks live in grant_region_admissible); a wrapping
    // window fails closed (returns true).
    bool grant_hits_reserved(uintptr_t base, size_t size);

    // Full admission policy for ONE prospective committed region (data or MMIO):
    //   size 0 / wrap                              -> refuse
    //   hits a reserved block (authorized too)     -> refuse   [Rule 7 core]
    //   memory type this chip cannot encode        -> refuse
    //   DEV : authorized caller + exactly-encodable + not a bit-band alias
    //   RAM : exactly encodable + confined to the user arena (every caller)
    //
    // `caller_authorized` is AUTH_MEMORY on the caller's authority cap, NOT
    // `Thread::privileged`. Only the DEV arm reads it (the RAM arm ignores it, Choice 10C).
    bool grant_region_admissible(uintptr_t base, size_t size, uint32_t attr,
                                 bool caller_authorized);

    // Boot self-check (KICKOS_ASSERT): every reserved block is well-formed and the static
    // grantable extents (arena + app code + appdata) are reserved-disjoint.
    void grant_reserved_validate(void);
#else
    inline bool grant_hits_reserved(uintptr_t, size_t) { return false; }
    // The memory-type arm survives with enforcement off; this is NOT a `return true`.
    inline bool grant_region_admissible(uintptr_t, size_t, uint32_t attr, bool)
    {
        return grant_nocache_admissible(attr);
    }
    inline void grant_reserved_validate(void) {}
#endif
}

#endif
