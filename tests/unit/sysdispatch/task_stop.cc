// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A kill of the task serving the published console ends that task, and with it the console, at
// once: before its members have run to their deaths, which a slay may not outlast. And a slay that
// waits for the death drops its own hold only, whoever has created the declaration again since.

#include <kickos/cap.h>
#include <kickos/endpoint.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include "kseam_test.h"
#include "syscall_internal.h"

using namespace kickos;
using namespace kickos::testfix;

namespace
{
    class TaskStop : public KSeam
    {
    };

    constexpr uint8_t PRIO_CREATOR = 8;
    constexpr uint8_t PRIO_MEMBER = 4;

    struct Reseat
    {
        Task* group;
        Thread* member;
        int decl_state;
        Task* successor;
    };
    Reseat g_reseat;

    // While the slay waits: the last member's exit, as another core runs it, and then a create of
    // the dead declaration by another creator, before the slayer runs again.
    void the_group_dies_and_is_created_again(Thread* slayer)
    {
        IrqLock lock;
        Task* const t = g_reseat.group;
        uint16_t const gen = task_gen(t);
        (void)task_release(t, lock);
        task_member_leave(g_reseat.member, kernel().threads.index_of(g_reseat.member));
        if (task_sweep_done(t))
        {
            thread_abort_park(slayer, 0, lock);
            task_report_death(t, gen);
        }
        size_t const decl = static_cast<size_t>(kernel().tasks().size() - 1
                                                - (t - kernel().tasks().base));
        g_reseat.decl_state = task_decl_state(decl);
        int err = 0;
        g_reseat.successor = task_create_declared(decl, 0, FIXTURE_TASK_TAG, nullptr, 0, 0u, nullptr,
                                                  &err, lock);
    }
}

TEST_F(TaskStop, a_kill_ends_the_console_task_before_its_member_runs)
{
    Thread* const creator = seat_pool(0, PRIO_CREATOR);
    attach_caps(creator, KICKOS_SEAM_CAP_WIDTH);
    {
        IrqLock lock;
        sched::reschedule(nullptr, lock);
    }
    ASSERT_EQ(sched::current(), creator);
    int err = 0;
    Task* served = nullptr;
    {
        IrqLock lock;
        served = kickos::testfix::task_held(kernel().threads.kill_tag_of(creator), &err, lock);
    }
    ASSERT_NE(served, nullptr) << "fixture: the create refused (" << err << ")";
    Thread* const member = seat_pool(1, PRIO_MEMBER);
    join_task(member, served);
    member->task_role = kickos::TASK_ROLE_ENTRY;
    Endpoint* const ep = endpoint();
    {
        IrqLock lock;
        int const handle = kernel().endpoints.handle_for(kernel().endpoints.index_of(ep));
        ASSERT_TRUE(cap_console_publish(creator, handle, lock));
        cap_console_serve(served, lock);
    }
    ASSERT_EQ(ep->console, EP_CONSOLE_SERVED) << "fixture: the console is not served";

    ASSERT_EQ(task_slay(task_handle(served), 0u), 0);
    EXPECT_NE(member->state, ThreadState::EXITED) << "fixture: the member already died";
    EXPECT_TRUE(task_ended(served)) << "the kill did not end the task";
    EXPECT_EQ(ep->console, EP_CONSOLE_ENDED) << "the console outlives the kill of its task";
}

TEST_F(TaskStop, a_slay_s_wait_drops_no_hold_of_the_next_creator_of_its_declaration)
{
    Thread* const creator = seat_pool(0, PRIO_CREATOR);
    attach_caps(creator, KICKOS_SEAM_CAP_WIDTH);
    {
        IrqLock lock;
        sched::reschedule(nullptr, lock);
    }
    ASSERT_EQ(sched::current(), creator);
    int err = 0;
    Task* group = nullptr;
    {
        IrqLock lock;
        group = kickos::testfix::task_held(kernel().threads.kill_tag_of(creator), &err, lock);
    }
    ASSERT_NE(group, nullptr) << "fixture: the create refused (" << err << ")";
    Thread* const member = seat_pool(1, PRIO_MEMBER);
    join_task(member, group);
    g_reseat = Reseat{group, member, -1, nullptr};
    uint32_t const holds = kernel().task_holds;
    kos_task_t const handle = task_handle(group);
    wake_next_park(the_group_dies_and_is_created_again);

    EXPECT_EQ(task_slay(handle, 1000u), 0);

    ASSERT_EQ(g_reseat.decl_state, 0) << "fixture: the dead declaration was not free to create";
    ASSERT_EQ(g_reseat.successor, group) << "fixture: the create took another record";
    IrqLock lock;
    EXPECT_TRUE(task_created_by(group, FIXTURE_TASK_TAG))
        << "the slay dropped the hold of the record's next creator";
    EXPECT_EQ(task_resolve(task_handle(group)), group);
    EXPECT_EQ(task_resolve(handle), nullptr) << "the dead incarnation's handle still names a task";
    EXPECT_EQ(kernel().task_holds, holds) << "one hold ended at the create and one began";
}
