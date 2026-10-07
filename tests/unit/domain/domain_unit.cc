// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The real kernel/domain/domain.cc: its handle codec, and the memory-type refusal of its RAM
// admission. The five arch and grant functions below are the seam domain_for calls.

#include <kickos/domain.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>

#include <kickos/sys/errno.h>

#include <gtest/gtest.h>

namespace
{
    int g_nocache = ARCH_MPU_NOCACHE_REFUSED;
}

extern "C" int arch_mpu_nocache_support(void)
{
    return g_nocache;
}

extern "C" size_t arch_mpu_min_region(void)
{
    return 32u;
}

extern "C" int arch_mpu_region_pow2(void)
{
    return 1;
}

namespace kickos
{
    bool grant_region_admissible(uintptr_t, size_t, uint32_t, bool)
    {
        return true;
    }

    bool memory_type_free(uintptr_t, size_t, uint32_t, Thread const*)
    {
        return true;
    }

    namespace
    {
        constexpr int SLOT = KICKOS_MAX_DOMAINS - 1;

        Domain* live_slot(uint16_t generation)
        {
            Domain* const d = &kernel().domains[SLOT];
            *d = Domain{};
            d->generation = generation;
            d->refcount = 1;
            return d;
        }
    }

    // A domain slot claimed 32768 times mints a handle with bit 31 set. It must still resolve,
    // or every capability naming that slot answers -KOS_EBADF until the generation wraps.
    TEST(DomainHandle, an_aged_handle_is_negative_and_resolves)
    {
        Domain* const d = live_slot(0x8000u);
        int const handle = domain_handle(d);

        EXPECT_LT(handle, 0) << "generation 0x8000 sets bit 31";
        EXPECT_EQ(domain_resolve(handle), d);
    }

    TEST(DomainHandle, the_last_generation_before_the_wrap_resolves)
    {
        Domain* const d = live_slot(0xFFFFu);
        EXPECT_EQ(domain_resolve(domain_handle(d)), d);
    }

    TEST(DomainHandle, a_stale_aged_handle_is_refused)
    {
        Domain* const d = live_slot(0x8000u);
        int const handle = domain_handle(d);
        d->generation++;

        EXPECT_EQ(domain_resolve(handle), nullptr);
    }

    TEST(DomainHandle, the_null_domains_handle_is_refused)
    {
        EXPECT_EQ(domain_handle(nullptr), -1);
        EXPECT_EQ(domain_resolve(-1), nullptr);
    }

    // The ABI's answer to a memory type the chip cannot honour is -KOS_ENOTSUP, on a region
    // backend as on a translating one.
    TEST(DomainAdmit, an_unhonoured_memory_type_answers_enotsup)
    {
        alignas(64) static char ram[64];
        g_nocache = ARCH_MPU_NOCACHE_REFUSED;
        int err = 0;

        EXPECT_EQ(domain_for(0u, ram, sizeof ram, ARCH_MPU_NOCACHE, nullptr, &err), nullptr);
        EXPECT_EQ(err, KOS_ENOTSUP);
    }

    TEST(DomainAdmit, an_honoured_memory_type_is_admitted)
    {
        alignas(64) static char ram[64];
        g_nocache = ARCH_MPU_NOCACHE_PROGRAMMED;
        int err = 0;

        EXPECT_NE(domain_for(0u, ram, sizeof ram, ARCH_MPU_NOCACHE, nullptr, &err), nullptr);
        EXPECT_EQ(err, 0);
    }
}
