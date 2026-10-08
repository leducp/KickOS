// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The frame pool's one entry point that hands frames out with their previous owner's bytes. A
// sync a reservation or a run owed dies with it when its frames go back to the pool, on the
// strength of every consumer writing every byte through the kernel's cacheable view before
// anything reads it or a task maps it (kernel-alias-maintained-over-uncached-memory,
// docs/reference/invariants.md).

#ifndef KICKOS_KERNEL_MEM_FRAME_POOL_UNWRITTEN_H
#define KICKOS_KERNEL_MEM_FRAME_POOL_UNWRITTEN_H

#include <kickos/arch/arch.h>

#include <stddef.h>

namespace kickos
{
    class UnwrittenFrames
    {
        // `pages` consecutive frames, or 0 when no run that long is free.
        static arch_phys_addr_t alloc_run(size_t pages);

        friend arch_phys_addr_t frame_pool_alloc_user_run(size_t pages);
        // Defined in aspace.cc alone.
        friend class AspaceUnwritten;
    };
}

#endif
