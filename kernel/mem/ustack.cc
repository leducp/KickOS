// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/ustack.h>

#if KICKOS_HAVE_ASPACE

#include <kickos/domain.h>
#include <kickos/frame_pool.h>
#include <kickos/vrange.h>

#include <stdint.h>

namespace kickos
{
    namespace
    {
        // The stack's frames plus the guard's: the page below a stack belongs to that stack,
        // so no later allocation can map it.
        size_t run_pages(size_t stack_pages)
        {
            return stack_pages + 1u;
        }
    }

    UserStack ustack_alloc(Domain* d, size_t want)
    {
        UserStack out;
        struct arch_aspace* const space = domain_space(d);
        VirtualRanges* const ranges = domain_ranges_mut(d);
        if (space == nullptr or ranges == nullptr or want == 0)
        {
            return out;
        }
        size_t const g = arch_aspace_granule();
        if (want > SIZE_MAX - g)
        {
            return out; // the round-up below would wrap
        }
        size_t const pages = (want + g - 1u) / g;
        // Cleared frames: nothing writes a fresh stack before the thread runs on it.
        arch_phys_addr_t const run = frame_pool_alloc_user_run(run_pages(pages));
        if (run == 0)
        {
            return out;
        }
        // The low frame of the run is the guard and is never mapped. The stack starts one
        // granule above it, so the first store below the stack's base has no translation.
        arch_phys_addr_t const stack = run + static_cast<arch_phys_addr_t>(g);
        uintptr_t const va = static_cast<uintptr_t>(stack);
        // BEFORE THE MAP, and over the guard as well as the stack: the record is what refuses
        // a later reservation here, and reserving after mapping would leave a mapping standing
        // on a refusal. Keyed on the RUN's base, which is the guard page.
        if (not ranges->reserve(static_cast<uintptr_t>(run), run_pages(pages), VR_USTACK))
        {
            frame_pool_free_run(run, run_pages(pages), g);
            return out;
        }
        if (arch_aspace_map(space, va, stack, pages, ARCH_MAP_R | ARCH_MAP_W,
                            ARCH_MAP_NORMAL) != ARCH_ASPACE_OK)
        {
            (void)ranges->release(static_cast<uintptr_t>(run));
            frame_pool_free_run(run, run_pages(pages), g);
            return out;
        }
        out.base = va;
        out.bytes = pages * g;
        return out;
    }

    void ustack_free(Domain* d, uintptr_t base, size_t bytes)
    {
        struct arch_aspace* const space = domain_space(d);
        if (space == nullptr or base == 0 or bytes == 0)
        {
            return;
        }
        size_t const g = arch_aspace_granule();
        size_t const pages = bytes / g;
        if (pages == 0 or base < g)
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
            // still maps these frames and destroy is what frees them. THE RECORD STAYS with
            // them, so nothing can be reserved over a window this space still maps.
            return;
        }
        VirtualRanges* const ranges = domain_ranges_mut(d);
        if (ranges != nullptr)
        {
            (void)ranges->release(base - g);
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
