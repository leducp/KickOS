// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The partition's user share: the top KICKOS_AMP_USER_SHARE_SIZE bytes of the region every node
// of an own-image AMP partition writes, above the kernel's own objects there, held by root as a
// reservation in the translating space's range list or the region backend's ownership table. A
// flat build keeps neither and seats nothing.

#ifndef KICKOS_AMPSHARE_H
#define KICKOS_AMPSHARE_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/arch/arch.h>
#include <kickos/config/amp_ports.h>
#include <kickos/sys/abi.h>

#if defined(KICKOS_AMP_OWN_IMAGE) && KICKOS_AMP_OWN_IMAGE && KICKOS_AMP_USER_SHARE_SIZE != 0
#define KICKOS_AMP_SHARE 1
#else
#define KICKOS_AMP_SHARE 0
#endif

namespace kickos
{
    struct Thread;
    class VirtualRanges;

#if KICKOS_AMP_SHARE
    constexpr uintptr_t AMP_SHARE_BASE = KICKOS_AMP_USER_SHARE_ADDR;
    constexpr size_t AMP_SHARE_SIZE = KICKOS_AMP_USER_SHARE_SIZE;

    // The share's one memory type, carried by every mapping of it, the kernel's own included.
    constexpr bool AMP_SHARE_UNCACHED = KICKOS_AMP_USER_SHARE_UNCACHED != 0;

    constexpr bool amp_share_meets(uint64_t lo, uint64_t hi)
    {
        return lo < AMP_SHARE_BASE + static_cast<uint64_t>(AMP_SHARE_SIZE) and AMP_SHARE_BASE < hi;
    }

    // Whether [base, base + size) lies inside the share. Bounds only: who may name it is the
    // reservation's question.
    constexpr bool amp_share_holds(uintptr_t base, size_t size)
    {
        uintptr_t const last = base + size - 1u;
        return size != 0 and last >= base and base >= AMP_SHARE_BASE
               and last <= AMP_SHARE_BASE + (AMP_SHARE_SIZE - 1u);
    }

    unsigned char* amp_share_kernel_view(void);

    // Must run before any peer is released: a write a peer makes first would be cleared.
    void amp_share_clear(void);

    // Panics rather than boot a root that cannot map the share.
    void amp_share_seat(Thread* root);

#if KICKOS_HAVE_ASPACE
    // A memory window's admission where it lies inside the share `own` holds, answered in `rc`,
    // a size of part granules refused; false where it does not, which leaves it to the
    // reservation rules.
    bool amp_share_window(VirtualRanges const* own, kos_window const& w, int* rc);
#endif

#if defined(KICKOS_ENABLE_SELFTEST)
    // False where root holds no reservation over the share.
    bool amp_share_seated(uintptr_t* base, size_t* size);
#endif
#endif
}

#endif
