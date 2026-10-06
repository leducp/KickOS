// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A kill of the task serving the published console ends that task, and with it the console, at
// once: before its members have run to their deaths, which a slay may not outlast.

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
}

TEST_F(TaskStop, a_kill_ends_the_console_task_before_its_member_runs)
{
    Thread* const creator = seat_pool(0, PRIO_CREATOR);
    attach_caps(creator, KICKOS_CAP_CHILD_WIDTH);
    {
        IrqLock lock;
        sched::reschedule();
    }
    ASSERT_EQ(sched::current(), creator);
    int err = 0;
    Task* served = nullptr;
    {
        IrqLock lock;
        served = task_create(kernel().threads.kill_tag_of(creator), 0u, nullptr, 0, 0u, nullptr,
                             &err);
    }
    ASSERT_NE(served, nullptr) << "fixture: task_create refused (" << err << ")";
    Thread* const member = seat_pool(1, PRIO_MEMBER);
    join_task(member, served);
    member->task_entry = true;
    Endpoint* const ep = endpoint();
    {
        IrqLock lock;
        int const handle = kernel().endpoints.handle_for(kernel().endpoints.index_of(ep));
        ASSERT_TRUE(cap_console_publish(creator, handle));
        cap_console_serve(served);
    }
    ASSERT_EQ(ep->console, EP_CONSOLE_SERVED) << "fixture: the console is not served";

    ASSERT_EQ(task_kill(task_handle(served)), 0);
    EXPECT_NE(member->state, ThreadState::EXITED) << "fixture: the member already died";
    EXPECT_TRUE(task_ended(served)) << "the kill did not end the task";
    EXPECT_EQ(ep->console, EP_CONSOLE_ENDED) << "the console outlives the kill of its task";
}
