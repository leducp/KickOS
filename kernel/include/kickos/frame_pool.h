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
    // A capability's unit over this pool: a RUN, the pool holding thousands of frames against
    // a spawned thread's single-digit capability table. `base` stays below the resolve
    // chokepoint, so no address reaches the capability ABI.
    struct FrameRun
    {
        arch_phys_addr_t base = 0;
        uint32_t pages = 0;
    };

    // Seat a frame RUN and take the creator's reference over frames the caller already holds.
    // -1 when the pool is full. NOT in cap.h: the base is an arch_phys_addr_t and that header
    // must not learn an address width.
    [[nodiscard]] int frame_run_create(arch_phys_addr_t base, uint32_t pages);

    // One reference on a frame RUN for a holder that is not a capability. A MAPPING is one:
    // without it the last capability's drop frees frames a live leaf still points at. False at
    // the ceiling or on a handle that does not resolve.
    [[nodiscard]] bool frame_run_ref(int obj_handle);
    void frame_run_release(int obj_handle);
    // Holders, capabilities and mappings alike. 0 when the handle does not resolve.
    uint8_t frame_run_refcount(int obj_handle);
    // The frame RUN slot a handle names, or -1. This is what a VirtualRange stores.
    int frame_run_slot_of(int obj_handle);

    // A teardown's release, named by the run's SLOT as the range recorded it. A slot out of
    // range, or one holding no reference, is a no-op rather than a drop of whatever sits there.
    // The slot is pinned by the very reference this drops.
    void frame_run_release_by_slot(int slot);

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
