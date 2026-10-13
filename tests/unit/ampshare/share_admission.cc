// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A region board's seat of the partition's user share in root, and the arena confinement's one
// exception for it, over a base+limit region seam this file sets.

#include <kickos/ampshare.h>
#include <kickos/grant.h>
#include <kickos/ramown.h>
#include <kickos/task.h>

#include <gtest/gtest.h>

#include "region_seam.h"

#include <stddef.h>
#include <stdint.h>

static_assert(KICKOS_AMP_SHARE, "this gate's posture states a share");

namespace
{
    constexpr size_t MIN_REGION = 32u;
    constexpr uintptr_t ARENA_BASE = 0x20000000u;
    constexpr size_t ARENA_SIZE = 0x3C000u;
    constexpr uintptr_t SHARE = kickos::AMP_SHARE_BASE;
    constexpr size_t SHARE_SIZE = kickos::AMP_SHARE_SIZE;
    constexpr uint32_t RW = ARCH_MPU_R | ARCH_MPU_W;
}

extern "C"
{
    void* arch_ram_alloc(size_t) { return nullptr; }
    size_t arch_domain_static_regions(struct arch_mpu_region*, size_t) { return 0; }
}

RegionSeam g_region_seam = {
    .ram_base = ARENA_BASE, .ram_size = ARENA_SIZE, .min_region = MIN_REGION};

namespace kickos
{
    namespace
    {
        Task g_tasks[2];
    }

    kos_task_t task_handle(Task const* t)
    {
        for (size_t i = 0; i < 2; i++)
        {
            if (&g_tasks[i] == t)
            {
                return (static_cast<kos_task_t>(t->gen) << TASK_INDEX_BITS)
                       | static_cast<kos_task_t>(i + 1u);
            }
        }
        return KOS_TASK_NONE;
    }
}

namespace
{
    using kickos::grant_region_admissible;
    using kickos::ram_owner_nameable;
    using kickos::ram_owner_seat;

    kickos::Task* root() { return &kickos::g_tasks[0]; }
    kickos::Task* stranger() { return &kickos::g_tasks[1]; }

    // The share sits outside the arena, so nothing but the exception admits it.
    TEST(AmpShare, the_share_lies_outside_the_arena)
    {
        EXPECT_GT(SHARE, ARENA_BASE + ARENA_SIZE - 1u);
    }

    TEST(AmpShare, a_window_inside_the_share_is_admitted)
    {
        EXPECT_TRUE(grant_region_admissible(SHARE, SHARE_SIZE, RW, false));
        EXPECT_TRUE(grant_region_admissible(SHARE + MIN_REGION, MIN_REGION, RW, false));
        EXPECT_TRUE(grant_region_admissible(SHARE + SHARE_SIZE - MIN_REGION, MIN_REGION, RW,
                                            false));
    }

    TEST(AmpShare, nothing_past_or_below_the_share_is)
    {
        EXPECT_FALSE(grant_region_admissible(SHARE, SHARE_SIZE + MIN_REGION, RW, false));
        EXPECT_FALSE(grant_region_admissible(SHARE + SHARE_SIZE, MIN_REGION, RW, false));
        EXPECT_FALSE(grant_region_admissible(SHARE - MIN_REGION, 2u * MIN_REGION, RW, false));
        EXPECT_FALSE(grant_region_admissible(SHARE - MIN_REGION, MIN_REGION, RW, false));
    }

    // The control: the arena's own confinement is unchanged.
    TEST(AmpShare, the_arena_is_still_admitted)
    {
        EXPECT_TRUE(grant_region_admissible(ARENA_BASE, MIN_REGION, RW, false));
        EXPECT_FALSE(grant_region_admissible(ARENA_BASE + ARENA_SIZE, MIN_REGION, RW, false));
    }

    // Seated once, for root: its holder names any part of it, and no other task names any.
    TEST(AmpShare, root_holds_the_seated_share_and_no_other_task_does)
    {
        EXPECT_FALSE(ram_owner_nameable(root(), SHARE + MIN_REGION, MIN_REGION));
        ASSERT_TRUE(ram_owner_seat(root(), SHARE, SHARE_SIZE));
        EXPECT_TRUE(ram_owner_nameable(root(), SHARE, SHARE_SIZE));
        EXPECT_TRUE(ram_owner_nameable(root(), SHARE + MIN_REGION, MIN_REGION));
        EXPECT_FALSE(ram_owner_nameable(root(), SHARE, SHARE_SIZE + 1u));
        EXPECT_FALSE(ram_owner_nameable(stranger(), SHARE + MIN_REGION, MIN_REGION));
        EXPECT_FALSE(ram_owner_nameable(stranger(), SHARE, SHARE_SIZE));
    }

    // An extent no region describes exactly is refused rather than recorded rounded.
    TEST(AmpShare, a_share_no_region_describes_is_not_seated)
    {
        EXPECT_FALSE(ram_owner_seat(root(), SHARE + 4u, SHARE_SIZE - MIN_REGION));
        EXPECT_FALSE(ram_owner_seat(root(), SHARE, SHARE_SIZE - 4u));
        EXPECT_FALSE(ram_owner_seat(nullptr, SHARE, SHARE_SIZE));
    }
}
