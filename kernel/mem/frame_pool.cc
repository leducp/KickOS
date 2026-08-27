// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The one FrameAllocator and the two callbacks a translating backend reaches it through.
// kickos/frame_pool.h carries the contract.

#include <kickos/frame_pool.h>

#if KICKOS_HAVE_ASPACE

#include <kickos/frame.h>
#include <kickos/irqlock.h>
#include <kickos/arch/arch.h>

#include <stdint.h>

extern "C"
{
    // The chip<->kernel contract of a translating board. NOT weak: a chip that selects
    // HAS_ASPACE and carves no pool must fail the LINK, an absent carve being a pool of
    // zero frames that reads to every caller as ordinary exhaustion.
    extern unsigned char __kickos_frame_pool_start[];
    extern unsigned char __kickos_frame_pool_end[];
    extern unsigned char __kickos_frame_pool_delta[];
}

namespace kickos
{
    namespace
    {
        FrameAllocator g_frames;
        size_t g_refused = 0;

        uintptr_t pool_delta()
        {
            return reinterpret_cast<uintptr_t>(__kickos_frame_pool_delta);
        }
    }

    bool frame_pool_init()
    {
        uintptr_t const base = reinterpret_cast<uintptr_t>(__kickos_frame_pool_start);
        uintptr_t const top = reinterpret_cast<uintptr_t>(__kickos_frame_pool_end);
        if (top <= base)
        {
            return false;
        }
        return g_frames.init(base, static_cast<size_t>(top - base), arch_aspace_granule());
    }

    size_t frame_pool_free()
    {
        return g_frames.frames_free();
    }

    size_t frame_pool_total()
    {
        return g_frames.frames_total();
    }

    size_t frame_pool_refused()
    {
        return g_refused;
    }

    void* frame_pool_ptr(arch_phys_addr_t frame)
    {
        uintptr_t const p = static_cast<uintptr_t>(frame) + pool_delta();
        if (not g_frames.is_allocated(p))
        {
            return nullptr;
        }
        return reinterpret_cast<void*>(p);
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
    // COUNTED rather than refused in silence: release() already protects the allocator from
    // a double free, and that protection is exactly what would hide a space destroyed while
    // it still mapped a frame it does not own.
    if (not kickos::g_frames.release(static_cast<uintptr_t>(frame) + kickos::pool_delta()))
    {
        kickos::g_refused++;
    }
}

}

#endif
