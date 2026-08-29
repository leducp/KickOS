// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The system's one FrameAllocator, over the carve the chip linker script reserves for it
// (__kickos_frame_pool_start/_end).
//
// Every entry point below takes the pool's IrqLock (nesting-safe), except frame_pool_init and
// frame_pool_total, which read state written once at boot.

#ifndef KICKOS_FRAME_POOL_H
#define KICKOS_FRAME_POOL_H

#include <kickos/arch/arch.h>

#include <stddef.h>

namespace kickos
{
    // Describes the carve at the granule the arch reports. False on a carve too small for a
    // bitmap plus one usable frame. A second call strands every frame handed out since the first.
    bool frame_pool_init();

    size_t frame_pool_free();
    size_t frame_pool_total();

    // How many times the pool has refused a free: a nonzero count is a frame freed twice or a
    // frame the pool never owned.
    size_t frame_pool_refused();

    // The pointer the kernel reaches a frame's bytes through, or null when `frame` is not one
    // this pool handed out.
    void* frame_pool_ptr(arch_phys_addr_t frame);

    // `pages` consecutive frames, or 0 when no run that long is free.
    // The bytes are the previous owner's: only a caller that overwrites every byte before
    // anything unprivileged can reach them may use this one; anything a task maps takes
    // frame_pool_alloc_user_run below.
    arch_phys_addr_t frame_pool_alloc_run(size_t pages);

    // The same run with every byte zero. Whole-run or nothing: a frame the kernel cannot reach
    // through its own alias fails the whole allocation and the answer is 0.
    arch_phys_addr_t frame_pool_alloc_user_run(size_t pages);

    // `granule` is the map editor's granule, which is what the run was measured in.
    void frame_pool_free_run(arch_phys_addr_t run, size_t pages, size_t granule);

#if defined(KICKOS_ENABLE_SELFTEST)
    // Refuse the `nth` next allocation, by either entry point, taking nothing; then disarm.
    // 0 disarms without refusing.
    void frame_pool_fail_in(size_t nth);

    // Whether an arming is still waiting for its attempt.
    bool frame_pool_fail_armed();
#endif
}

#endif
