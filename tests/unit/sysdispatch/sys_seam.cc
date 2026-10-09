// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// See sys_seam.h.

#include <kickos/arch/arch.h>
#include <kickos/kernel.h>

#include "sys_seam.h"

using kickos::testfix::SYS_GRANULE;

namespace
{
    kickos::testfix::WindowHook g_hook = nullptr;
    uint32_t g_hook_ordinal = 0;
    bool g_in_hook = false;
}

namespace kickos
{
    namespace testfix
    {
        uint32_t g_syncs = 0;
        uint32_t g_windows = 0;

        unsigned char* user_bytes(uintptr_t va)
        {
            uintptr_t const page = va & ~static_cast<uintptr_t>(SYS_GRANULE - 1u);
            unsigned char* const bytes = host_pool_bytes(page, 1);
            if (bytes == nullptr)
            {
                return nullptr;
            }
            return bytes + (va - page);
        }

        void on_window(WindowHook fn)
        {
            g_hook = fn;
            g_hook_ordinal = 0;
            g_in_hook = false;
        }

        void sys_seam_reset()
        {
            host_pool_reset_counters();
            host_aspace_reset();
            g_syncs = 0;
            g_windows = 0;
            on_window(nullptr);
        }
    }
}

extern "C"
{
    void arch_aspace_window_area(uintptr_t* base, size_t* size)
    {
        *base = static_cast<uintptr_t>(0x400000000000ull);
        *size = static_cast<size_t>(1ull << 30);
    }

    void arch_dcache_invalidate(void*, size_t)
    {
        kickos::testfix::g_syncs++;
    }

    void arch_irq_window(void)
    {
        kickos::testfix::g_windows++;
        if (g_hook == nullptr or g_in_hook)
        {
            return;
        }
        g_in_hook = true;
        g_hook_ordinal++;
        g_hook(g_hook_ordinal);
        g_in_hook = false;
    }

    size_t arch_mpu_min_region(void)
    {
        return 32u;
    }

    int arch_mpu_region_pow2(void)
    {
        return 0;
    }

    uintptr_t arch_ram_base(void)
    {
        return 0;
    }

    size_t arch_ram_size(void)
    {
        return 0;
    }
}
