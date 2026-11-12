// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A caller-named thread stack against the partition's user share on a region board, over the
// REAL ram_stack_admit (kernel/thread/thread.cc), which a spawn's stack takes, and the REAL
// grant and ownership rules under it. Root holds the share, so only the stack rule can refuse.

#include <kickos/ampshare.h>
#include <kickos/domain.h>
#include <kickos/grant.h>
#include <kickos/ramown.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <gtest/gtest.h>

#include "region_seam.h"

#include <stddef.h>
#include <stdint.h>
#include "ram_seam.h"

static_assert(KICKOS_AMP_SHARE, "this gate's posture states a share");

namespace
{
    constexpr size_t MIN_REGION = 32u;
    constexpr uintptr_t ARENA_BASE = 0x20000000u;
    constexpr size_t ARENA_SIZE = 0x3C000u;
    constexpr uintptr_t SHARE = kickos::AMP_SHARE_BASE;
    constexpr size_t SHARE_SIZE = kickos::AMP_SHARE_SIZE;
    constexpr size_t STACK = 1024u;
}

extern "C"
{
    void* arch_ram_alloc(size_t) { return nullptr; }
    size_t arch_domain_static_regions(struct arch_mpu_region*, size_t, size_t) { return 0; }
}

RegionSeam g_region_seam = {
    .ram_base = ARENA_BASE, .ram_size = ARENA_SIZE, .min_region = MIN_REGION};

namespace kickos
{
    size_t image_count(void)
    {
        return 1;
    }

    size_t domain_region_count(Domain const*) { return 0; }
    arch_mpu_region const* domain_region_at(Domain const*, size_t) { return nullptr; }

    namespace
    {
        Task g_root_task;
    }

    kos_task_t task_handle(Task const* t)
    {
        if (t == &g_root_task)
        {
            return static_cast<kos_task_t>(1u);
        }
        return KOS_TASK_NONE;
    }
}

namespace
{
    struct AmpShareStack : ::testing::Test
    {
        kickos::Thread root;

        void SetUp() override
        {
            root.task = &kickos::g_root_task;
            static bool seated = false;
            if (not seated)
            {
                ASSERT_TRUE(kickos::ram_owner_seat(root.task, SHARE, SHARE_SIZE));
                ASSERT_TRUE(kickos::ram_owner_seat(root.task, ARENA_BASE, STACK));
                seated = true;
            }
        }
    };

    // The control: the share's holder still grants any part of it.
    TEST_F(AmpShareStack, root_grants_part_of_the_share)
    {
        EXPECT_EQ(kickos::ram_region_admit(&root, SHARE, STACK, ARCH_MPU_R | ARCH_MPU_W,
                                           kickos::RAM_ADMIT_GRANT),
                  0);
    }

    TEST_F(AmpShareStack, a_stack_inside_the_share_is_refused)
    {
        EXPECT_EQ(kickos::ram_stack_admit(&root, SHARE, STACK), -KOS_EPERM);
        EXPECT_EQ(kickos::ram_stack_admit(&root, SHARE + SHARE_SIZE - STACK, STACK), -KOS_EPERM);
    }

    TEST_F(AmpShareStack, a_stack_in_root_s_own_arena_block_is_admitted)
    {
        EXPECT_EQ(kickos::ram_stack_admit(&root, ARENA_BASE, STACK), 0);
    }
}
