// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The partition's user share as a mapping of its one memory type that always exists, over the
// REAL type rule of the posture's backend: aspace_frames_type_ok (kernel/mem/aspace.cc) under
// translation, memory_type_free (kernel/thread/thread.cc) under region descriptors. No space
// and no thread maps anything here, so only the share can refuse.

#include <kickos/ampshare.h>
#include <kickos/aspace.h>
#include <kickos/domain.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>

#include <gtest/gtest.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

static_assert(KICKOS_AMP_SHARE, "this gate's posture states a share");

namespace
{
    constexpr uintptr_t SHARE = kickos::AMP_SHARE_BASE;
    constexpr size_t SHARE_SIZE = kickos::AMP_SHARE_SIZE;
}

#if KICKOS_HAVE_ASPACE
extern "C"
{
    size_t arch_aspace_granule(void) { return 4096u; }
    uintptr_t arch_aspace_user_offset(void) { return 0; }
    arch_phys_addr_t arch_aspace_frame_at(struct arch_aspace*, uintptr_t) { return 0; }
}
#endif

namespace kickos
{
#if KICKOS_HAVE_ASPACE
    struct arch_aspace* domain_space(Domain const*) { return nullptr; }
    VirtualRanges const* domain_ranges(Domain const*) { return nullptr; }
#else
    size_t domain_region_count(Domain const*) { return 0; }
    arch_mpu_region const* domain_region_at(Domain const*, size_t) { return nullptr; }
#endif
}

namespace
{
    constexpr size_t G = 4096u;

#if KICKOS_HAVE_ASPACE and KICKOS_AMP_USER_SHARE_UNCACHED
    constexpr uint8_t SHARE_TYPE = ARCH_MAP_NOCACHE;
    constexpr uint8_t OTHER_TYPE = ARCH_MAP_NORMAL;
    static_assert(kickos::AMP_SHARE_UNCACHED, "this posture's share is uncached");
#elif KICKOS_HAVE_ASPACE
    constexpr uint8_t SHARE_TYPE = ARCH_MAP_NORMAL;
    constexpr uint8_t OTHER_TYPE = ARCH_MAP_NOCACHE;
    static_assert(not kickos::AMP_SHARE_UNCACHED, "this posture's share is cached");
#endif
#if KICKOS_HAVE_ASPACE

    bool admits(uintptr_t base, size_t size, uint8_t type)
    {
        return kickos::aspace_frames_type_ok(base, size / G, type, nullptr);
    }
#else
    constexpr uint32_t SHARE_TYPE = ARCH_MPU_R | ARCH_MPU_W;
    constexpr uint32_t OTHER_TYPE = ARCH_MPU_R | ARCH_MPU_W | ARCH_MPU_NOCACHE;
    static_assert(not kickos::AMP_SHARE_UNCACHED, "this posture's share is cached");

    bool admits(uintptr_t base, size_t size, uint32_t type)
    {
        return kickos::memory_type_free(base, size, type, nullptr);
    }
#endif

    TEST(AmpShareType, the_share_s_own_type_is_admitted_over_any_part)
    {
        EXPECT_TRUE(admits(SHARE, SHARE_SIZE, SHARE_TYPE));
        EXPECT_TRUE(admits(SHARE + G, G, SHARE_TYPE));
        EXPECT_TRUE(admits(SHARE + SHARE_SIZE - G, G, SHARE_TYPE));
    }

    TEST(AmpShareType, the_other_type_is_refused_over_any_part)
    {
        EXPECT_FALSE(admits(SHARE, SHARE_SIZE, OTHER_TYPE));
        EXPECT_FALSE(admits(SHARE + G, G, OTHER_TYPE));
        EXPECT_FALSE(admits(SHARE + SHARE_SIZE - G, G, OTHER_TYPE));
    }

    TEST(AmpShareType, a_run_meeting_either_edge_carries_the_share_s_type)
    {
        EXPECT_FALSE(admits(SHARE - G, 2u * G, OTHER_TYPE));
        EXPECT_FALSE(admits(SHARE + SHARE_SIZE - G, 2u * G, OTHER_TYPE));
    }

    TEST(AmpShareType, either_type_is_admitted_beside_the_share)
    {
        EXPECT_TRUE(admits(SHARE - G, G, OTHER_TYPE));
        EXPECT_TRUE(admits(SHARE - G, G, SHARE_TYPE));
        EXPECT_TRUE(admits(SHARE + SHARE_SIZE, G, OTHER_TYPE));
        EXPECT_TRUE(admits(SHARE + SHARE_SIZE, G, SHARE_TYPE));
    }
}
