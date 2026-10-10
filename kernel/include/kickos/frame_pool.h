// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The system's one FrameAllocator, over the carve the chip linker script reserves for it
// (__kickos_frame_pool_start/_end).
//
// Every frame_pool_* entry point takes the pool's IrqLock (nesting-safe), except frame_pool_init,
// frame_pool_phys_bounds and frame_pool_alias, which read state written once at boot.

#ifndef KICKOS_FRAME_POOL_H
#define KICKOS_FRAME_POOL_H

#include <kickos/arch/arch.h>

#include <kickos/sys/abi.h> // kos_task_t

#include <stddef.h>

namespace kickos
{
    struct Task;

    // A capability's unit over this pool: a run, the pool holding thousands of frames against
    // a spawned thread's single-digit capability table. No capability carries `base`: the
    // address a map answers is the run's frames plus the user offset, as kos_ram_alloc's is.
    struct FrameRun
    {
        arch_phys_addr_t base = 0;
        uint32_t pages = 0;
        // The task charged for the run, for its whole life, wherever it is mapped and whoever
        // holds it. KOS_TASK_NONE charges no task.
        kos_task_t minter = KOS_TASK_NONE;
        // A mapping of another type than cacheable has been installed since the last sync of
        // these frames, so a cacheable one owes them one (aspace_cap_map).
        bool sync_owed = false;
    };

    // What frame_run_create answers when it could not make one, and what a caller holding no
    // run stores. A handle, so compare against it and never test the sign (slotpool.h).
    constexpr int FRAME_RUN_NONE = -1;

    // Seat a frame run and take the creator's reference over frames the caller already holds.
    // -1 when the pool is full. Not in cap.h: the base is an arch_phys_addr_t and that header
    // must not learn an address width.
    [[nodiscard]] int frame_run_create(arch_phys_addr_t base, uint32_t pages);

    // Seat a run naming no frame yet, charged to `minter`, with the creator's reference. No
    // capability resolves it until frame_run_fill. FRAME_RUN_NONE when the pool is full.
    [[nodiscard]] int frame_run_create_empty(kos_task_t minter);
    // Hand an empty run its frames. False, changing nothing, unless the run is live and empty.
    bool frame_run_fill(int obj_handle, arch_phys_addr_t base, uint32_t pages);
    // Whether `t` may mint one more run: fewer than KICKOS_TASK_FRAME_RUN_BUDGET live runs
    // charged to it. Caller holds IrqLock.
    bool frame_run_admit(Task const* t);

    // One reference on a frame run for a holder that is not a capability. A mapping is one:
    // without it the last capability's drop frees frames a live leaf still points at. False at
    // the ceiling or on a handle that does not resolve; true means frame_run_slot_of resolves
    // it for as long as the caller's IrqLock is held.
    [[nodiscard]] bool frame_run_ref(int obj_handle);
    void frame_run_release(int obj_handle);
    // The frame run slot a handle names, or -1; a VirtualRange stores it plus one.
    int frame_run_slot_of(int obj_handle);

    // FrameRun::sync_owed of the run a handle names; false when it does not resolve.
    bool frame_run_sync_owed(int obj_handle);
    void frame_run_set_sync_owed(int obj_handle, bool owed);

    // A teardown's release, named by the run's slot as the range recorded it. A slot out of
    // range, or one holding no reference, is a no-op rather than a drop of whatever sits there.
    // The slot is pinned by the very reference this drops.
    void frame_run_release_by_slot(int slot);

    // Describes the carve at the granule the arch reports. False on a carve too small for a
    // bitmap plus one usable frame. A second call strands every frame handed out since the first.
    bool frame_pool_init();

    size_t frame_pool_free();

    // The pointer the kernel reaches a frame's bytes through, or null when `frame` is not one
    // this pool handed out.
    void* frame_pool_ptr(arch_phys_addr_t frame);
    // The same pointer for any frame of the image's own memory, allocated or not.
    void* frame_pool_alias(arch_phys_addr_t frame);

    // `pages` consecutive frames with every byte zero, or 0 when no run that long is free.
    // Whole-run or nothing: a frame the kernel cannot reach through its own alias fails the
    // whole allocation and the answer is 0.
    arch_phys_addr_t frame_pool_alloc_user_run(size_t pages);

    // `granule` is the map editor's granule, which is what the run was measured in.
    void frame_pool_free_run(arch_phys_addr_t run, size_t pages, size_t granule);

    // The physical frames the pool describes, [*lo, *hi), bitmap included.
    void frame_pool_phys_bounds(arch_phys_addr_t* lo, arch_phys_addr_t* hi);
}

#endif
