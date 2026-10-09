// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The frame pool for a host gate over a translating kernel: the kernel's pool entry points
// over a host array, with every frame accounted for. Keep this header independent of GTest.

#ifndef KICKOS_TESTS_UNIT_COMMON_HOST_FRAME_POOL_H
#define KICKOS_TESTS_UNIT_COMMON_HOST_FRAME_POOL_H

#include <kickos/arch/arch.h>

#include <stddef.h>
#include <stdint.h>

namespace kickos
{
    namespace testfix
    {
        constexpr size_t HOST_POOL_GRANULE = 4096;
        constexpr size_t HOST_POOL_FRAMES = 128;
        constexpr arch_phys_addr_t HOST_POOL_LO = 0x40000000u;

        // `pages` consecutive free frames, or 0. Every entry point allocates through this, and
        // each call is one attempt the countdown below can refuse.
        arch_phys_addr_t host_pool_take(size_t pages);

        // Refuse the nth attempt from now, taking nothing; 0 refuses none.
        void host_pool_fail_in(size_t nth);
        // An arming still waiting for its attempt.
        bool host_pool_fail_armed();
        size_t host_pool_attempts();

        size_t host_pool_used();
        bool host_pool_owns(arch_phys_addr_t pa);
        // The bytes behind [pa, pa + pages granules), allocated or not; null outside the pool.
        unsigned char* host_pool_bytes(arch_phys_addr_t pa, size_t pages);

        // Frees of a pool frame not allocated, and of a frame no pool owns. Both must stay 0.
        size_t host_pool_double_frees();
        size_t host_pool_outside_frees();

        // The countdown off and every counter at 0; the frames stay as they are.
        void host_pool_reset_counters();

        // Which frames are allocated now, to free what a case took since.
        struct HostPoolMark
        {
            bool used[HOST_POOL_FRAMES];
        };
        HostPoolMark host_pool_mark();
        void host_pool_free_since(HostPoolMark const& mark);
    }
}

#endif
