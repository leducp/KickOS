// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The frame pool's one entry point that hands frames out with their previous owner's bytes,
// private to kernel/mem: every consumer writes every byte of every frame through the kernel's
// cacheable view before anything reads it or a task maps it. A sync a reservation or a run
// owed dies with it when its frames go back to the pool, on the strength of that overwrite
// (kernel-alias-maintained-over-uncached-memory, docs/reference/invariants.md).

#ifndef KICKOS_KERNEL_MEM_FRAME_POOL_UNWRITTEN_H
#define KICKOS_KERNEL_MEM_FRAME_POOL_UNWRITTEN_H

#include <kickos/arch/arch.h>

#include <stddef.h>

namespace kickos
{
    // `pages` consecutive frames, or 0 when no run that long is free.
    arch_phys_addr_t frame_pool_alloc_run(size_t pages);
}

#endif
