// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A spawn's delegated endpoint copy carries the source's badge WHOLE. The grant list is staged on
// the spawner's syscall descent, so a stager holding the badge narrower than its stored width
// would hand the child the low bits of another identity, with nothing refused.

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
    // The params and the grant list a spawn names lie in a region the spawner holds.
    struct alignas(kickos::testfix::REGION_MIN) GrantBlock
    {
        kos_thread_params p;
        kos_cap_grant grant;
        kos_thread_t out;
    };
    GrantBlock g_grants;

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
            class SpawnBadge : public KSeam
            {
              protected:
                void SetUp() override
                {
                    KSeam::SetUp();
                    g_arena_used = 0;
                }
            };

            // Bits in every byte, so a narrowed stager keeps a value a reader could mistake for
            // a badge of its own.
            constexpr uint32_t BADGE = 0xA5C3E10Fu;
        }

        TEST_F(SpawnBadge, a_delegated_endpoint_copy_carries_the_whole_badge)
        {
            Kernel& k = kernel();
            Task* const own = task(0);
            Thread* const spawner = seat_pool(2, 5);
            join_task(spawner, own);
            attach_caps(spawner, KICKOS_SEAM_CAP_WIDTH);
            k.current(kickos_kernel_core()) = spawner;
            ASSERT_TRUE(spawner->mpu.add(reinterpret_cast<uintptr_t>(&g_grants), sizeof(g_grants),
                                         ARCH_MPU_R | ARCH_MPU_W));

            Endpoint* const ep = endpoint();
            int const idx = k.endpoints.index_of(ep);
            int const obj = k.endpoints.handle_for(idx);
            uint32_t src = KCAP_INVALID;
            {
                IrqLock lock;
                k.endpoints.refs()[idx] = 1;
                CapEntry const* const e =
                    cap_install_at(spawner, KICKOS_CAP_FIRST_DYNAMIC, obj, CapType::CAP_ENDPOINT,
                                   CAP_SIGNAL | CAP_TRANSFER, BADGE);
                ASSERT_NE(e, nullptr);
                src = (static_cast<uint32_t>(e->gen) << KCAP_INDEX_BITS) | KICKOS_CAP_FIRST_DYNAMIC;
            }

            kos_thread_params& p = g_grants.p;
            p = {};
            p.entry = entry;
            p.name = "badged";
            p.prio = 1;
            g_grants.grant.source_cap = src;
            g_grants.grant.rights_mask = KOS_CAP_SIGNAL;
            p.caps = &g_grants.grant;
            p.cap_count = 1;
            ASSERT_EQ(thread_create_call(&p, reinterpret_cast<uintptr_t>(&g_grants.out)), 0)
                << "fixture: the spawn was refused";
            kos_thread_t const out = g_grants.out;

            Thread* child = nullptr;
            {
                IrqLock lock;
                child = k.threads.resolve(out);
            }
            ASSERT_NE(child, nullptr);
            CapEntry const* const ce = cap_slot(child->caps, KOS_SPAWN_DELEGATED_CAP0);
            ASSERT_EQ(ce->type, static_cast<uint8_t>(CapType::CAP_ENDPOINT));
            EXPECT_EQ(ce->rights, static_cast<uint8_t>(CAP_SIGNAL));
            EXPECT_EQ(cap_badge(*ce), BADGE) << "the stager narrowed the delegated badge";
        }
    }
}
