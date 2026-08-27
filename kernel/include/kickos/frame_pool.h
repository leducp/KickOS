// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The system's one FrameAllocator, over the carve the chip linker script reserves for it
// (__kickos_frame_pool_start/_end). The backend never keeps a pool of its own, so destroy's
// accounting is checkable against these counters.
//
// Compiled to nothing where no translating backend exists (KICKOS_HAVE_ASPACE), which is
// what keeps a region-descriptor board's .bss where it was.

#ifndef KICKOS_FRAME_POOL_H
#define KICKOS_FRAME_POOL_H

#include <kickos/arch/arch.h>

#include <stddef.h>

namespace kickos
{
    // Describes the carve at the granule the arch reports. False on a carve too small for a
    // bitmap plus one usable frame, or where the linker script reserved none. Idempotent,
    // and calling it twice would strand every frame handed out since the first call.
    bool frame_pool_init();

    size_t frame_pool_free();
    size_t frame_pool_total();

    // How many times the pool has REFUSED a free. A backend only ever hands back what the
    // pool gave it, so a nonzero count is a frame freed twice or a frame the pool never
    // owned, and both are silent inside the callback: the counter is what a gate reads to
    // see them.
    size_t frame_pool_refused();

    // The pointer the kernel reaches a frame's bytes through, or null when `frame` is not one
    // this pool handed out. SCOPED TO THE CARVE the linker described: it is not a map of
    // physical memory, and a backend's own map is a different thing that answers more.
    void* frame_pool_ptr(arch_phys_addr_t frame);

    // `pages` CONSECUTIVE frames, or 0 when no run that long is free. Every frame of the run
    // is released one at a time through kickos_frame_free, so the pool's accounting and its
    // refusal counter stay the only ones there are.
    arch_phys_addr_t frame_pool_alloc_run(size_t pages);

#if defined(KICKOS_ENABLE_SELFTEST)
    // FORCED FAILURE, and the only injection in the tree. Refuse the `nth` next allocation,
    // by either entry point, taking nothing; then disarm. 0 disarms without refusing.
    //
    // Every unwind arm under a create is written and unreachable otherwise: an exhausted
    // pool is not a state a test can produce on a board sized for the image, and without
    // this each arm is code nothing ever ran. Walking `nth` up from 1 reaches
    // them one at a time, so a refusal is attributable to ONE allocation rather than to a
    // pool that ran dry somewhere.
    void frame_pool_fail_in(size_t nth);

    // Whether an arming is still waiting for its attempt. A sweep reads this to tell "the
    // injected attempt refused" from "the sequence ended before reaching it", which is what
    // says the sweep has covered every allocation the path makes.
    bool frame_pool_fail_armed();
#endif
}

#endif
