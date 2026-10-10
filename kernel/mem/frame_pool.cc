// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/frame_pool.h>

#include "frame_pool_unwritten.h"

#if KICKOS_HAVE_ASPACE

#include <kickos/frame.h>
#include <kickos/irqlock.h>
#include <kickos/kruntime.h>
#include <kickos/arch/arch.h>

#include <stdint.h>

extern "C"
{
    // Defined by the chip linker script; a HAS_ASPACE chip that states none fails the link.
    extern unsigned char __kickos_frame_pool_delta[];
}

namespace kickos
{
    namespace
    {
        FrameAllocator g_frames;
        uintptr_t g_base = 0;
        uintptr_t g_top = 0;

        // volatile keeps this a relocated word; a plain constant folds back into each caller.
        unsigned char* const volatile g_pool_delta = __kickos_frame_pool_delta;

        uintptr_t pool_delta()
        {
            return reinterpret_cast<uintptr_t>(g_pool_delta);
        }
    }

    bool frame_pool_init()
    {
        uintptr_t base = 0;
        uintptr_t top = 0;
        arch_frame_pool_bounds(&base, &top);
        g_base = base;
        g_top = top;
        if (top <= base)
        {
            return false;
        }
        return g_frames.init(base, static_cast<size_t>(top - base), arch_aspace_granule());
    }

    void frame_pool_phys_bounds(arch_phys_addr_t* lo, arch_phys_addr_t* hi)
    {
        *lo = static_cast<arch_phys_addr_t>(g_base - pool_delta());
        *hi = static_cast<arch_phys_addr_t>(g_top - pool_delta());
    }

    size_t frame_pool_free()
    {
        IrqLock lock;
        return g_frames.frames_free();
    }

    arch_phys_addr_t UnwrittenFrames::alloc_run(size_t pages)
    {
        IrqLock lock;
        uintptr_t const p = g_frames.alloc_run(pages);
        if (p == 0)
        {
            return 0;
        }
        return static_cast<arch_phys_addr_t>(p - pool_delta());
    }

    arch_phys_addr_t frame_pool_alloc_user_run(size_t pages)
    {
        arch_phys_addr_t const run = UnwrittenFrames::alloc_run(pages);
        if (run == 0)
        {
            return 0;
        }
        size_t const g = arch_aspace_granule();
        // The frames are already this caller's, so the loop needs no lock of its own.
        for (size_t i = 0; i < pages; i++)
        {
            void* const p = frame_pool_ptr(run + static_cast<arch_phys_addr_t>(i * g));
            if (p == nullptr)
            {
                frame_pool_free_run(run, pages, g);
                return 0;
            }
            kmemset(p, 0, g);
        }
        return run;
    }

    void frame_pool_free_run(arch_phys_addr_t run, size_t pages, size_t granule)
    {
        for (size_t i = 0; i < pages; i++)
        {
            kickos_frame_free(run + static_cast<arch_phys_addr_t>(i * granule));
        }
    }

    void* frame_pool_alias(arch_phys_addr_t frame)
    {
        return reinterpret_cast<void*>(static_cast<uintptr_t>(frame) + pool_delta());
    }

    void* frame_pool_ptr(arch_phys_addr_t frame)
    {
        IrqLock lock;
        void* const p = frame_pool_alias(frame);
        if (not g_frames.is_allocated(reinterpret_cast<uintptr_t>(p)))
        {
            return nullptr;
        }
        return p;
    }
}

extern "C"
{

arch_phys_addr_t kickos_frame_alloc(void)
{
    kickos::IrqLock lock;
    uintptr_t const p = kickos::g_frames.alloc();
    if (p == 0)
    {
        return 0;
    }
    return static_cast<arch_phys_addr_t>(p - kickos::pool_delta());
}

void kickos_frame_free(arch_phys_addr_t frame)
{
    kickos::IrqLock lock;
    (void)kickos::g_frames.release(static_cast<uintptr_t>(frame) + kickos::pool_delta());
}

}

#endif
