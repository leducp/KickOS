// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Rule 7 (docs/design-m4-driver-model.md sec.7): the grant path REFUSES a region
// that overlaps a kernel-reserved block, turning single-ownership from "trust the
// granter" into "the kernel refuses". The reserved set is the arch's owns-for-life
// peripherals (timebase, IRQ controller, MPU, clock/reset gates); each enforcing
// chip declares it via arch_reserved_blocks (arch.h).
//
// Body compiled only under KICKOS_HAVE_MPU (grant.cc); with no enforcement the
// inline stubs below make every call a zero-flash no-op, so the call sites stay
// #if-free: hits_reserved never fires, admissible always passes, validate is empty.

#ifndef KICKOS_GRANT_H
#define KICKOS_GRANT_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/config.h> // KICKOS_HAVE_MPU

// Fill target for arch_reserved_blocks: the most blocks any chip declares (mk64f
// = 4 today). A fixed stack buffer, so no allocation on the grant hot path.
#define KICKOS_MAX_RESERVED 8u

namespace kickos
{
#if KICKOS_HAVE_MPU
    // True iff [base, base+size) touches any reserved block -- or, on a bit-band
    // chip, the alias image of a reserved block that lies in the aliasable 1 MB
    // peripheral region. Pure. size 0 touches nothing (shape checks live in
    // grant_region_admissible); a wrapping window fails closed (returns true).
    bool grant_hits_reserved(uintptr_t base, size_t size);

    // Full admission policy for ONE prospective committed region (data or MMIO).
    // See grant.cc for the baked-in conditions; the short of it:
    //   size 0 / wrap                              -> refuse
    //   hits a reserved block (privileged too)     -> refuse   [Rule 7 core]
    //   DEV : privileged caller + exactly-encodable + not a bit-band alias
    //   RAM : naturally aligned + confined to the user arena (every caller)
    bool grant_region_admissible(uintptr_t base, size_t size, uint32_t attr,
                                 bool caller_privileged);

    // Boot self-check: every reserved block is well-formed and the static grantable
    // extents (arena + app code + appdata) are reserved-disjoint. A boot diagnostic
    // (KICKOS_ASSERT), not a hot path.
    void grant_reserved_validate(void);
#else
    inline bool grant_hits_reserved(uintptr_t, size_t) { return false; }
    inline bool grant_region_admissible(uintptr_t, size_t, uint32_t, bool) { return true; }
    inline void grant_reserved_validate(void) {}
#endif
}

#endif
