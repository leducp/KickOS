// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The arena and the region unit under the real system-call layer at the preset's region
// posture. The arena hands out blocks of a host array the gate fills beforehand, and the
// region unit rounds a block to 256 bytes, so a block's extent passes the bytes it was asked
// for.

#include <string.h>

#include <kickos/arch/arch.h>
#include <kickos/domain.h>

#include "region_seam.h"

namespace kickos
{
    namespace testfix
    {
        alignas(256) unsigned char g_arena[ARENA_BYTES];
        size_t g_arena_used = 0;
    }

    Domain* domain_kernel(void)
    {
        return nullptr;
    }

    size_t domain_region_count(Domain const* d)
    {
        if (d == nullptr)
        {
            return 0;
        }
        return d->region_count;
    }

    arch_mpu_region const* domain_region_at(Domain const* d, size_t i)
    {
        return &d->regions[i];
    }
}

extern "C"
{
    void* arch_ram_alloc(size_t size)
    {
        using kickos::testfix::g_arena;
        using kickos::testfix::g_arena_used;
        size_t const extent = arch_ram_region_size(size);
        if (g_arena_used + extent > sizeof(g_arena))
        {
            return nullptr;
        }
        void* const p = &g_arena[g_arena_used];
        g_arena_used += extent;
        return p;
    }

    uintptr_t arch_ram_base(void)
    {
        return reinterpret_cast<uintptr_t>(kickos::testfix::g_arena);
    }

    size_t arch_ram_size(void)
    {
        return sizeof(kickos::testfix::g_arena);
    }

    size_t arch_mpu_min_region(void)
    {
        return kickos::testfix::REGION_MIN;
    }

    int arch_mpu_region_pow2(void)
    {
        return 1;
    }

    bool arch_mpu_region_encodable(uintptr_t, size_t size)
    {
        return size != 0;
    }

    int arch_mpu_nocache_support(void)
    {
        return ARCH_MPU_NOCACHE_ALREADY;
    }

    size_t arch_domain_static_regions(struct arch_mpu_region*, size_t)
    {
        return 0;
    }

    void arch_mpu_apply_now(struct arch_mpu_region const*, size_t, struct arch_mpu_encoded const*)
    {
    }
}
