// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/aspace.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/sched.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include "aspace_seam.h"
#include "kseam_test.h"

#include <stdint.h>

#if not KICKOS_LIBC_REENT or not KICKOS_HAVE_ASPACE
#error "this gate's posture is a translating backend with the libc reentrant seam on"
#endif

using namespace kickos;
using namespace kickos::testfix;

namespace
{
    struct Write
    {
        struct arch_aspace* space = nullptr;
        uintptr_t dst = 0;
        Thread const* running = nullptr;
        bool seated = false;
    };

    constexpr int WRITES = 8;
    Write g_writes[WRITES];
    int g_write_count = 0;

    void record(struct arch_aspace* space, uintptr_t dst)
    {
        if (g_write_count >= WRITES)
        {
            ADD_FAILURE() << "more reent writes than the gate records";
            return;
        }
        Thread const* const c = sched::current();
        g_writes[g_write_count] = Write{space, dst, c, aspace_seated_for(c)};
        g_write_count++;
    }
}

namespace kickos
{
    bool ep_copy(UserOwner dspace, uintptr_t dst, UserOwner, uintptr_t, size_t)
    {
        record(dspace, dst);
        return true;
    }
}

namespace
{
    constexpr uint8_t PRIO_LOW = 4;
    constexpr uint8_t PRIO_MID = 5;
    constexpr uint8_t PRIO_HIGH = 6;

    constexpr size_t STATE_BYTES = 64;
    unsigned char g_state[3][STATE_BYTES];

    class ReentSeat : public KSeam
    {
      protected:
        void SetUp() override
        {
            KSeam::SetUp();
            aspace_seam_reset();
            g_write_count = 0;
        }

        void TearDown() override
        {
            for (int i = 0; i < g_write_count; i++)
            {
                EXPECT_TRUE(g_writes[i].seated)
                    << "write " << i << " for a thread whose space was not installed";
            }
        }

        static Thread* fresh(int slot, uint8_t prio, Task* group)
        {
            Thread* const t = seat_pool(slot, prio);
            if (group != nullptr)
            {
                join_task(t, group);
            }
            t->reent = g_state[slot];
            t->reent_fresh = true;
            return t;
        }

        static void switch_now()
        {
            IrqLock lock;
            sched::reschedule(nullptr, lock);
        }
    };

    // The spaceless thread runs over the member's root, still installed on this core.
    TEST_F(ReentSeat, a_spaceless_switch_in_writes_no_reent_state)
    {
        Task* const group = task(0);
        ASSERT_NE(group, nullptr);
        (void)fresh(0, PRIO_MID, group);
        switch_now();
        int const before = g_write_count;

        Thread* const spaceless = fresh(1, PRIO_HIGH, nullptr);
        switch_now();
        ASSERT_EQ(sched::current(), spaceless) << "the spaceless thread never switched in";
        ASSERT_EQ(installed_on(0), space_of(task_domain(group)))
            << "fixture: the case needs another task's root left installed under it";
        EXPECT_EQ(g_write_count, before) << "a switch-in wrote reent state for a thread with no "
                                            "space, through the root left installed";
        EXPECT_TRUE(spaceless->reent_fresh);
    }

    TEST_F(ReentSeat, members_of_two_tasks_are_each_primed_through_their_own_space)
    {
        Task* const first = task(0);
        Task* const second = task(1);
        ASSERT_NE(first, nullptr);
        ASSERT_NE(second, nullptr);
        Thread* const a = fresh(0, PRIO_LOW, first);
        switch_now();
        Thread* const b = fresh(1, PRIO_MID, second);
        switch_now();
        ASSERT_EQ(sched::current(), b);

        ASSERT_EQ(g_write_count, 2);
        EXPECT_EQ(g_writes[0].running, a);
        EXPECT_EQ(g_writes[0].space, space_of(task_domain(first)));
        EXPECT_EQ(g_writes[0].dst, reinterpret_cast<uintptr_t>(a->reent));
        EXPECT_FALSE(a->reent_fresh);
        EXPECT_EQ(g_writes[1].running, b);
        EXPECT_EQ(g_writes[1].space, space_of(task_domain(second)));
    }
}
