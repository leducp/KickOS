// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The system's one FrameAllocator, over the carve the chip linker script reserves for it
// (__kickos_frame_pool_start/_end). The backend never keeps a pool of its own.
//
// Compiled to nothing where no translating backend exists (KICKOS_HAVE_ASPACE).
//
// Every entry point below takes the pool's IrqLock, the questions that only read the bitmap
// included: the bitmap and the free count are the allocator's live state, and a reader
// outside the lock reads a word another core is editing. The lock is nesting-safe. Each
// exception is stated where it is.

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

    // How many times the pool has refused a free. A backend only ever hands back what the
    // pool gave it, so a nonzero count is a frame freed twice or a frame the pool never
    // owned, and both are silent inside the callback.
    size_t frame_pool_refused();

    // The pointer the kernel reaches a frame's bytes through, or null when `frame` is not one
    // this pool handed out. Scoped to the carve the linker described.
    void* frame_pool_ptr(arch_phys_addr_t frame);

    // `pages` consecutive frames, or 0 when no run that long is free. Every frame of the run
    // is released one at a time through kickos_frame_free.
    arch_phys_addr_t frame_pool_alloc_run(size_t pages);

    // Frees the run at `run`, one frame at a time through that same callback. `granule` is
    // the map editor's granule, which is what the run was measured in.
    void frame_pool_free_run(arch_phys_addr_t run, size_t pages, size_t granule);

#if defined(KICKOS_ENABLE_SELFTEST)
    // Forced failure, and the only injection in the tree. Refuse the `nth` next allocation,
    // by either entry point, taking nothing; then disarm. 0 disarms without refusing.
    void frame_pool_fail_in(size_t nth);

    // Whether an arming is still waiting for its attempt: what tells "the injected attempt
    // refused" from "the sequence ended before reaching it".
    bool frame_pool_fail_armed();
#endif
}

#endif
