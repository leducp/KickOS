// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A spawn seats the far standard output the composition names, or the published console, on the
// child's capability 0, or is refused: a child with no capability 0 would write this node's
// kernel console instead, or nothing once a console is published.

#include <kickos/cap.h>
#include <kickos/endpoint.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include "kseam_test.h"
#include "region_seam.h"
#include "syscall_internal.h"

namespace
{
    // The params a spawn names lie in a region the spawner holds.
    struct alignas(kickos::testfix::REGION_MIN) ParamBlock
    {
        kos_thread_params p;
        kos_thread_t out;
    };
    ParamBlock g_far;

    constexpr kos_thread_t UNWRITTEN = 0x5A5A5A5Au;

    void entry(void*) {}
}

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            // The arena is the seam's and outlives reset(), so an earlier case's stacks are given
            // back here.
            class SpawnStdout : public KSeam
            {
              protected:
                void SetUp() override
                {
                    KSeam::SetUp();
                    g_arena_used = 0;
                }

                // The spawner, its own capability 0 seated on endpoint `*obj` as the boot seats a
                // far standard output, the endpoint's references then set to `refs`.
                Thread* far_spawner(int* obj, uint8_t refs)
                {
                    Kernel& k = kernel();
                    Thread* const spawner = seat_pool(2, 5);
                    join_task(spawner, task(0));
                    attach_caps(spawner, KICKOS_SEAM_CAP_WIDTH);
                    k.current(kickos_kernel_core()) = spawner;
                    EXPECT_TRUE(spawner->mpu.add(reinterpret_cast<uintptr_t>(&g_far), sizeof(g_far),
                                                 ARCH_MPU_R | ARCH_MPU_W));
                    Endpoint* const ep = endpoint();
                    int const idx = k.endpoints.index_of(ep);
                    *obj = k.endpoints.handle_for(idx);
                    IrqLock lock;
                    k.endpoints.refs()[idx] = 1;
                    EXPECT_TRUE(cap_stdout_far_seat(spawner, *obj, lock));
                    k.endpoints.refs()[idx] = refs;
                    return spawner;
                }

                // The spawner, the console published on endpoint `*obj` with the spawner as its
                // publisher, the endpoint's references then set to `refs`.
                Thread* console_spawner(int* obj, uint8_t refs)
                {
                    Kernel& k = kernel();
                    Thread* const spawner = seat_pool(2, 5);
                    join_task(spawner, task(0));
                    attach_caps(spawner, KICKOS_SEAM_CAP_WIDTH);
                    k.current(kickos_kernel_core()) = spawner;
                    EXPECT_TRUE(spawner->mpu.add(reinterpret_cast<uintptr_t>(&g_far), sizeof(g_far),
                                                 ARCH_MPU_R | ARCH_MPU_W));
                    Endpoint* const ep = endpoint();
                    int const idx = k.endpoints.index_of(ep);
                    *obj = k.endpoints.handle_for(idx);
                    IrqLock lock;
                    k.endpoints.refs()[idx] = 1;
                    EXPECT_TRUE(cap_console_publish(spawner, *obj, lock));
                    k.endpoints.refs()[idx] = refs;
                    return spawner;
                }

                int spawn()
                {
                    kos_thread_params& p = g_far.p;
                    p = {};
                    p.entry = entry;
                    p.name = "far";
                    p.prio = 1;
                    g_far.out = UNWRITTEN;
                    return thread_create_call(&p, reinterpret_cast<uintptr_t>(&g_far.out));
                }
            };
        }

        TEST_F(SpawnStdout, a_child_is_seated_on_the_far_standard_output)
        {
            int obj = 0;
            (void)far_spawner(&obj, 2);
            ASSERT_EQ(spawn(), 0);
            Thread* child = nullptr;
            {
                IrqLock lock;
                child = kernel().threads.resolve(g_far.out);
            }
            ASSERT_NE(child, nullptr);
            CapEntry const* const out = cap_slot(child->caps, KOS_CAP_STDOUT);
            ASSERT_EQ(out->type, static_cast<uint8_t>(CapType::CAP_ENDPOINT));
            EXPECT_EQ(cap_obj(*out), obj);
            EXPECT_EQ(kernel().endpoints.refs()[kernel().endpoints.live_index(obj)], 3u);
        }

        TEST_F(SpawnStdout, a_far_standard_output_at_its_reference_ceiling_refuses_the_spawn)
        {
            int obj = 0;
            (void)far_spawner(&obj, UINT8_MAX);
            EXPECT_EQ(spawn(), -KOS_EOVERFLOW) << "the child would fall back on the kernel console";
            EXPECT_EQ(g_far.out, UNWRITTEN);
            EXPECT_EQ(kernel().endpoints.refs()[kernel().endpoints.live_index(obj)], UINT8_MAX);
        }

        TEST_F(SpawnStdout, a_child_is_seated_on_the_published_console)
        {
            int obj = 0;
            (void)console_spawner(&obj, 2);
            ASSERT_EQ(spawn(), 0);
            Thread* child = nullptr;
            {
                IrqLock lock;
                child = kernel().threads.resolve(g_far.out);
            }
            ASSERT_NE(child, nullptr);
            CapEntry const* const out = cap_slot(child->caps, KOS_CAP_STDOUT);
            ASSERT_EQ(out->type, static_cast<uint8_t>(CapType::CAP_ENDPOINT));
            EXPECT_EQ(cap_obj(*out), obj);
            EXPECT_EQ(kernel().endpoints.refs()[kernel().endpoints.live_index(obj)], 3u);
        }

        TEST_F(SpawnStdout, a_published_console_at_its_reference_ceiling_refuses_the_spawn)
        {
            int obj = 0;
            (void)console_spawner(&obj, UINT8_MAX);
            EXPECT_EQ(spawn(), -KOS_EOVERFLOW) << "the child would have no standard output";
            EXPECT_EQ(g_far.out, UNWRITTEN);
            EXPECT_EQ(kernel().endpoints.refs()[kernel().endpoints.live_index(obj)], UINT8_MAX);
        }
    }
}
