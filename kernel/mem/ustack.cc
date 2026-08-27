// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// kickos/ustack.h carries the contract.

#include <kickos/ustack.h>

#if KICKOS_HAVE_ASPACE

#include <kickos/frame_pool.h>

#include <stdint.h>

namespace kickos
{
    namespace
    {
        // The stack's frames plus the guard's. The guard is charged to the pool, so the page
        // below a stack belongs to that stack and no later allocation can map it (F7 budgets
        // one granule per thread).
        size_t run_pages(size_t stack_pages)
        {
            return stack_pages + 1u;
        }
    }

    UserStack ustack_alloc(struct arch_aspace* space, size_t want)
    {
        UserStack out;
        if (space == nullptr or want == 0)
        {
            return out;
        }
        size_t const g = arch_aspace_granule();
        if (want > SIZE_MAX - g)
        {
            return out; // the round-up below would wrap
        }
        size_t const pages = (want + g - 1u) / g;
        arch_phys_addr_t const run = frame_pool_alloc_run(run_pages(pages));
        if (run == 0)
        {
            return out;
        }
        // The low frame of the run is the guard and is never mapped. The stack starts one
        // granule above it, so the first store below the stack's base has no translation.
        arch_phys_addr_t const stack = run + static_cast<arch_phys_addr_t>(g);
        uintptr_t const va = static_cast<uintptr_t>(stack);
        if (arch_aspace_map(space, va, stack, pages, ARCH_MAP_R | ARCH_MAP_W,
                            ARCH_MAP_NORMAL) != ARCH_ASPACE_OK)
        {
            frame_pool_free_run(run, run_pages(pages), g);
            return out;
        }
        out.base = va;
        out.bytes = pages * g;
        return out;
    }

    void ustack_free(struct arch_aspace* space, uintptr_t base, size_t bytes)
    {
        if (space == nullptr or base == 0 or bytes == 0)
        {
            return;
        }
        size_t const g = arch_aspace_granule();
        size_t const pages = bytes / g;
        if (pages == 0)
        {
            return;
        }
        enum arch_aspace_result const rc = arch_aspace_unmap(space, base, pages);
        // The guard frame is never mapped, so no space will ever free it and it comes back
        // here whatever the unmap did.
        kickos_frame_free(static_cast<arch_phys_addr_t>(base) - g);
        if (rc != ARCH_ASPACE_OK)
        {
            // The unmap is total-or-fail, so a refusal left every entry standing: the space
            // still maps these frames and destroy is what frees them (F10). Freeing them
            // here as well would be a double release.
            return;
        }
        frame_pool_free_run(static_cast<arch_phys_addr_t>(base), pages, g);
    }

    // ustack_alloc maps the run at va == its own physical base, which is what lets a
    // stack address be handed to the pool as a frame here and in ustack_free.
    void* ustack_kptr(uintptr_t base)
    {
        if (base == 0)
        {
            return nullptr;
        }
        return frame_pool_ptr(static_cast<arch_phys_addr_t>(base));
    }
}

#endif
