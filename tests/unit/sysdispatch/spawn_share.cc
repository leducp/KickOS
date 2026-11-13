// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A spawn naming its stack inside the partition's user share on a region board, through the REAL
// system-call path. The spawner holds the share and may grant any part of it, so only the
// spawn's stack rule refuses: a call site admitting the stack as a plain grant passes it.

#include <kickos/ampshare.h>
#include <kickos/domain.h>
#include <kickos/kernel.h>
#include <kickos/ramown.h>
#include <kickos/sched.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include "kseam_test.h"
#include "region_seam.h"
#include "syscall_internal.h"

static_assert(KICKOS_AMP_SHARE, "this gate's posture states a share");

namespace
{
    constexpr size_t STACK = 0x1000u;
    // Clear of the arena's low blocks, which the spawn's kernel stack is carved from.
    constexpr uintptr_t BESIDE = KSEAM_ARENA_AT + 0x8000u;

    // The params a spawn names lie in a region the spawner holds.
    struct alignas(kickos::testfix::REGION_MIN) ParamBlock
    {
        kos_thread_params p;
        kos_thread_t out;
    };
    ParamBlock g_block;

    void entry(void*) {}
}

namespace kickos
{
    namespace testfix
    {
        static_assert(AMP_SHARE_BASE >= BESIDE + STACK
                          and AMP_SHARE_BASE + AMP_SHARE_SIZE <= KSEAM_ARENA_AT + ARENA_BYTES,
                      "the share must lie in the arena above a stack of its own, or the arena "
                      "bound refuses what the stack rule is meant to");

        namespace
        {
            class SpawnShare : public KSeam
            {
            };

            int spawn_on(uintptr_t stack)
            {
                kos_thread_params& p = g_block.p;
                p = {};
                p.entry = entry;
                p.name = "onshare";
                p.prio = 1;
                p.stack_base = reinterpret_cast<void*>(stack);
                p.stack_size = STACK;
                g_block.out = KOS_THREAD_NONE;
                return thread_create_call(&p, reinterpret_cast<uintptr_t>(&g_block.out));
            }
        }

        TEST_F(SpawnShare, a_stack_in_the_share_is_refused_and_one_beside_it_admitted)
        {
            ASSERT_EQ(reinterpret_cast<uintptr_t>(g_arena), static_cast<uintptr_t>(KSEAM_ARENA_AT))
                << "fixture: the link did not place the arena";
            Kernel& k = kernel();
            Task* const own = task(0);
            Thread* const spawner = seat_pool(2, 5);
            join_task(spawner, own);
            cap_seat_authority(spawner, AUTH_MEMORY | AUTH_TASKS);
            k.current(kickos_kernel_core()) = spawner;
            ASSERT_TRUE(spawner->mpu.add(reinterpret_cast<uintptr_t>(&g_block), sizeof(g_block),
                                         ARCH_MPU_R | ARCH_MPU_W));
            ASSERT_TRUE(ram_owner_seat(own, AMP_SHARE_BASE, AMP_SHARE_SIZE));
            ASSERT_TRUE(ram_owner_seat(own, BESIDE, STACK));
            ASSERT_EQ(ram_region_admit(spawner, AMP_SHARE_BASE, STACK, ARCH_MPU_R | ARCH_MPU_W,
                                       RAM_ADMIT_GRANT),
                      0)
                << "fixture: the holder cannot grant the share, so nothing below is the stack rule";

            EXPECT_EQ(spawn_on(AMP_SHARE_BASE), -KOS_EPERM);
            EXPECT_EQ(g_block.out, KOS_THREAD_NONE);
            EXPECT_EQ(spawn_on(AMP_SHARE_BASE + AMP_SHARE_SIZE - STACK), -KOS_EPERM);

            EXPECT_EQ(spawn_on(BESIDE), 0) << "the same spawn beside the share was refused";
            EXPECT_NE(g_block.out, KOS_THREAD_NONE);
        }
    }
}
