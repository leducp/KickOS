// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Placement in the region every node of an AMP partition writes.
//
// An object two kernels share cannot be an ordinary static: each image would allocate its own
// and both would link. It is placed here by name, at an address two link scripts derive from one
// partition geometry (arch/common/amp_partition.ld.h).
//
// The chip script places these sections NOLOAD: a region carrying bytes is written at load time,
// when a peer may already have published into it. Outside every image's .bss, no C runtime
// clears it; arch_amp_shared_zero does, from the chip's reset path before arch_init.
//
// A chip with no such section fails the link (--orphan-handling=error).

#ifndef KICKOS_ARCH_AMP_SHARED_H
#define KICKOS_ARCH_AMP_SHARED_H

#include <kickos/arch/arch.h>

#if defined(KICKOS_AMP_OWN_IMAGE) && KICKOS_AMP_OWN_IMAGE
#define KICKOS_AMP_SHARED(sub) __attribute__((section(".amp_shared." sub), used))

// The bounds the chip's link script defines around the sections above.
extern "C" unsigned char __kickos_amp_shared_start[];
extern "C" unsigned char __kickos_amp_shared_end[];

// The partition primary alone, before its own first write here. Nothing else clears this
// region: on a start whose RAM is not already zero the window layer seeds its inbox from a
// stale tail and the doorbell reads a peer's seat as standing while that peer is not running.
// A peer must not run it, having booted after the primary published.
static inline void arch_amp_shared_zero(void)
{
    if (KICKOS_AMP_NODE_ID != 0)
    {
        return;
    }
    for (unsigned char* p = __kickos_amp_shared_start; p < __kickos_amp_shared_end; p++)
    {
        *p = 0;
    }
}
#else
#define KICKOS_AMP_SHARED(sub)
#define arch_amp_shared_zero() ((void)0)
#endif

#endif
