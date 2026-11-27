// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// See host_frame_pool.h.

#include "host_frame_pool.h"

#include <kickos/frame_pool.h>

#include "../../../kernel/mem/frame_pool_unwritten.h"

#include <string.h>

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            constexpr size_t G = HOST_POOL_GRANULE;

            alignas(HOST_POOL_GRANULE) unsigned char g_bytes[HOST_POOL_FRAMES * G];
            bool g_used[HOST_POOL_FRAMES] = {};
            // Taken by kickos_frame_alloc: a table, as the kernel counts them.
            bool g_table[HOST_POOL_FRAMES] = {};
            size_t g_fail_in = 0;
            size_t g_attempts = 0;
            size_t g_double_frees = 0;
            size_t g_outside_frees = 0;

            size_t index_of(arch_phys_addr_t pa)
            {
                return static_cast<size_t>((pa - HOST_POOL_LO) / G);
            }

            bool refuse_this_attempt()
            {
                g_attempts++;
                if (g_fail_in == 0)
                {
                    return false;
                }
                g_fail_in--;
                return g_fail_in == 0;
            }
        }

        arch_phys_addr_t host_pool_take(size_t pages)
        {
            if (refuse_this_attempt() or pages == 0)
            {
                return 0;
            }
            for (size_t i = 0; i + pages <= HOST_POOL_FRAMES; i++)
            {
                bool free = true;
                for (size_t j = 0; free and j < pages; j++)
                {
                    free = not g_used[i + j];
                }
                if (free)
                {
                    for (size_t j = 0; j < pages; j++)
                    {
                        g_used[i + j] = true;
                    }
                    return HOST_POOL_LO + static_cast<arch_phys_addr_t>(i * G);
                }
            }
            return 0;
        }

        void host_pool_fail_in(size_t nth)
        {
            g_fail_in = nth;
        }

        bool host_pool_fail_armed()
        {
            return g_fail_in != 0;
        }

        size_t host_pool_attempts()
        {
            return g_attempts;
        }

        size_t host_pool_used()
        {
            size_t n = 0;
            for (bool const u : g_used)
            {
                if (u)
                {
                    n++;
                }
            }
            return n;
        }

        bool host_pool_owns(arch_phys_addr_t pa)
        {
            return pa >= HOST_POOL_LO and index_of(pa) < HOST_POOL_FRAMES;
        }

        unsigned char* host_pool_bytes(arch_phys_addr_t pa, size_t pages)
        {
            if (not host_pool_owns(pa) or index_of(pa) + pages > HOST_POOL_FRAMES)
            {
                return nullptr;
            }
            return &g_bytes[index_of(pa) * G];
        }

        size_t host_pool_double_frees()
        {
            return g_double_frees;
        }

        size_t host_pool_outside_frees()
        {
            return g_outside_frees;
        }

        void host_pool_reset_counters()
        {
            g_fail_in = 0;
            g_attempts = 0;
            g_double_frees = 0;
            g_outside_frees = 0;
        }

        HostPoolMark host_pool_mark()
        {
            HostPoolMark m;
            memcpy(m.used, g_used, sizeof(m.used));
            return m;
        }

        void host_pool_free_since(HostPoolMark const& mark)
        {
            for (size_t i = 0; i < HOST_POOL_FRAMES; i++)
            {
                if (g_used[i] and not mark.used[i])
                {
                    g_used[i] = false;
                }
            }
        }
    }

    arch_phys_addr_t UnwrittenFrames::alloc_run(size_t pages)
    {
        return testfix::host_pool_take(pages);
    }

    arch_phys_addr_t frame_pool_alloc_user_run(size_t pages)
    {
        arch_phys_addr_t const run = testfix::host_pool_take(pages);
        if (run != 0)
        {
            memset(testfix::host_pool_bytes(run, pages), 0, pages * testfix::G);
        }
        return run;
    }

    void frame_pool_free_run(arch_phys_addr_t run, size_t pages)
    {
        for (size_t i = 0; i < pages; i++)
        {
            kickos_leaf_free(run + static_cast<arch_phys_addr_t>(i * testfix::HOST_POOL_GRANULE));
        }
    }

    // Outside the pool a frame is the host address it names, as the image's own frames are.
    void* frame_pool_alias(arch_phys_addr_t frame)
    {
        if (testfix::host_pool_owns(frame))
        {
            return testfix::host_pool_bytes(frame, 1);
        }
        return reinterpret_cast<void*>(static_cast<uintptr_t>(frame));
    }

    void* frame_pool_ptr(arch_phys_addr_t frame)
    {
        if (not testfix::host_pool_owns(frame) or not testfix::g_used[testfix::index_of(frame)])
        {
            return nullptr;
        }
        return testfix::host_pool_bytes(frame, 1);
    }

    size_t frame_pool_free()
    {
        return testfix::HOST_POOL_FRAMES - testfix::host_pool_used();
    }

    size_t frame_pool_tables()
    {
        size_t n = 0;
        for (size_t i = 0; i < testfix::HOST_POOL_FRAMES; i++)
        {
            if (testfix::g_used[i] and testfix::g_table[i])
            {
                n++;
            }
        }
        return n;
    }

    void frame_pool_phys_bounds(arch_phys_addr_t* lo, arch_phys_addr_t* hi)
    {
        *lo = testfix::HOST_POOL_LO;
        *hi = testfix::HOST_POOL_LO + testfix::HOST_POOL_FRAMES * testfix::G;
    }
}

extern "C"
{
    // The kernel's (kernel/mem/aspace.cc) where the gate links it.
    __attribute__((weak)) arch_phys_addr_t kickos_presync_table_take(void)
    {
        return 0;
    }

    arch_phys_addr_t kickos_frame_alloc(void)
    {
        arch_phys_addr_t const reserved = kickos_presync_table_take();
        if (reserved != 0)
        {
            return reserved;
        }
        arch_phys_addr_t const frame = kickos::testfix::host_pool_take(1);
        if (frame != 0)
        {
            kickos::testfix::g_table[kickos::testfix::index_of(frame)] = true;
        }
        return frame;
    }

    void kickos_frame_free(arch_phys_addr_t frame)
    {
        kickos_leaf_free(frame);
    }

    void kickos_leaf_free(arch_phys_addr_t frame)
    {
        using namespace kickos::testfix;
        if (not host_pool_owns(frame))
        {
            g_outside_frees++;
            return;
        }
        if (not g_used[index_of(frame)])
        {
            g_double_frees++;
            return;
        }
        g_used[index_of(frame)] = false;
        g_table[index_of(frame)] = false;
    }
}
