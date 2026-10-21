// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A spawn refused because its MPU would decide an overlap otherwise than the kernel hands back
// everything it claimed: the reclaimed slot EXITED at its generation and its region set
// untouched, the kernel stack on the free list, and the task it built held by nobody.

#include <kickos/domain.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include "kseam_test.h"
#include "region_seam.h"
#include "syscall_internal.h"

extern "C" uint64_t syscall_dispatch(uintptr_t nr, uintptr_t a0, uintptr_t a1, uintptr_t a2,
                                     uintptr_t a3);

#if not KICKOS_MEMORY_ENFORCED or not KICKOS_HAVE_MPU or KICKOS_HAVE_ASPACE
#error "this gate refuses a spawn over a region set"
#endif

namespace
{
    unsigned char g_code[64];

    // The params a spawn names lie in a region the spawner holds.
    struct alignas(kickos::testfix::REGION_MIN) ParamBlock
    {
        kos_thread_params p;
    };
    ParamBlock g_block;
}

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class SpawnUnwind : public KSeam
            {
            };

            void entry(void*) {}

            // The domain_for of the K-seam hands out its first free slot.
            Domain* next_domain()
            {
                for (int i = 0; i < FIXTURE_DOMAIN_SLOTS; i++)
                {
                    if (g_domain_refs[i] == 0 and not g_domain_live[i])
                    {
                        return &g_domains[i];
                    }
                }
                return nullptr;
            }

            TEST_F(SpawnUnwind, an_overlap_refusal_hands_back_everything_it_claimed)
            {
                Kernel& k = kernel();
                ASSERT_EQ(ARCH_MPU_OVERLAP, ARCH_MPU_OVERLAP_HIGHER);

                // A data grant of the spawner's own, so the child builds a task of its own.
                Task* const own = task(0);
                Thread* const spawner = seat_pool(2, 5);
                join_task(spawner, own);
                cap_seat_authority(spawner, AUTH_MEMORY | AUTH_TASKS);
                k.current(kickos_kernel_core()) = spawner;
                ASSERT_TRUE(spawner->mpu.add(reinterpret_cast<uintptr_t>(&g_block),
                                             sizeof(g_block), ARCH_MPU_R | ARCH_MPU_W));
                uintptr_t const grant = syscall_dispatch(KOS_SYS_RAM_ALLOC, REGION_MIN, 0, 0, 0);
                ASSERT_NE(grant, 0u);

                // Slot 1 is reclaimable, with a region set of its own left from its occupant.
                Thread& was = k.threads.slots[1];
                was.state.to<ThreadState::EXITED>();
                was.mpu.clear();
                uintptr_t const mark = reinterpret_cast<uintptr_t>(g_code);
                ASSERT_TRUE(was.mpu.add(mark, sizeof(g_code), ARCH_MPU_R));
                // At the wrap, so the claim carries into the high half the revert must undo too.
                k.threads.gen[1] = 0xFFFFu;
                uint16_t const gen = k.threads.gen[1];
                uint16_t const gen_hi = k.threads.gen_hi[1];

                // The child's task data reads the block its kernel stack will be: read-only
                // under a writable stack is decided writable by the higher region.
                void* const stack = &g_arena[g_arena_used];
                Domain* const dom = next_domain();
                ASSERT_NE(dom, nullptr);
                dom->regions[0] = {reinterpret_cast<uintptr_t>(stack),
                                   arch_ram_region_size(KICKOS_USER_STACK_SIZE), ARCH_MPU_R};
                dom->region_count = 1;

                kos_thread_params& p = g_block.p;
                p = {};
                p.entry = entry;
                p.name = "refused";
                p.prio = 1;
                p.mem_base = reinterpret_cast<void*>(grant);
                p.mem_size = REGION_MIN;
                kos_thread_t out = 0;
                EXPECT_EQ(thread_create_call(&p, &out), -KOS_EINVAL);
                EXPECT_EQ(out, KOS_THREAD_NONE);

                EXPECT_EQ(was.state, ThreadState::EXITED);
                EXPECT_EQ(k.threads.gen[1], gen) << "the slot kept the claim's generation";
                EXPECT_EQ(k.threads.gen_hi[1], gen_hi) << "the slot kept the claim's high half";
                ASSERT_EQ(was.mpu.end() - was.mpu.begin(), 1) << "the slot's set was written";
                EXPECT_EQ(was.mpu.begin()->base, mark);

                EXPECT_EQ(k.threads.stack_pop(), stack) << "the kernel stack was not handed back";
                EXPECT_EQ(k.threads.stack_pop(), nullptr);

                // Liveness is intrinsic on a region backend: a task and a domain nobody holds
                // are free slots.
                EXPECT_EQ(dom->refcount, 0u) << "the built task's domain is held";
                for (int i = 0; i < KICKOS_MAX_TASKS; i++)
                {
                    Task const& t = k.tasks[i];
                    if (&t != own)
                    {
                        EXPECT_EQ(t.refcount, 0u) << "task " << i << " is held";
                        EXPECT_EQ(t.creator_tag, ThreadPool::KILL_TAG_NONE) << "task " << i;
                    }
                }
            }
        }
    }
}
