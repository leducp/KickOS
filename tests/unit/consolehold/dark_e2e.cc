// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The dark window end to end: the REAL kernel/init/console.cc deciding the console is dark, the
// REAL kernel/thread/park.cc parking the writer, and the reclaim or a publish in console.cc
// waking it, over the K-seam with nothing of the console stubbed but the device.

#include <string.h>

#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#include <kickos/cap.h>
#include <kickos/console_tx.h>
#include <kickos/endpoint.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <kickos/sys/errno.h>

#include "dark_seam.h"
#include "kseam_test.h"

using namespace kickos;
using namespace kickos::testfix;

namespace
{
    constexpr uint8_t PRIO_WRITER = 8;

    class ConsoleDark : public KSeam
    {
    };

    Thread* g_writer = nullptr;
    uint8_t g_park_kind = WAIT_NONE;

    // The console published for a task of its own, its handover done, and that task ended while
    // a thread of it still holds the register window: dark. The writer is running and its stdout
    // names the console.
    void stage_dark()
    {
        darkseam::reset();
        g_park_kind = WAIT_NONE;
        g_writer = seat_pool(0, PRIO_WRITER);
        attach_caps(g_writer, KICKOS_CAP_CHILD_WIDTH);
        {
            IrqLock lock;
            sched::reschedule(nullptr, lock);
        }
        ASSERT_EQ(sched::current(), g_writer);
        Endpoint* const ep = endpoint();
        Task* const served = task(0);
        {
            IrqLock lock;
            int const handle = kernel().endpoints.handle_for(kernel().endpoints.index_of(ep));
            ASSERT_TRUE(cap_console_publish(g_writer, handle, lock));
            cap_console_serve(served, lock);
            console_handover_begin();
        }
        console_owner_set_user();
        darkseam::g_window_free = false;
        {
            IrqLock lock;
            task_end(served, 0, true, lock);
        }
        ASSERT_NE(console_dark(), 0) << "fixture: the console is not dark";
    }

    void run_isolated(void (*body)())
    {
        fflush(nullptr);
        pid_t const pid = fork();
        ASSERT_NE(pid, -1) << "fork failed, the arm did not run";
        if (pid == 0)
        {
            body();
            fflush(nullptr);
            if (::testing::Test::HasFailure())
            {
                _exit(1);
            }
            _exit(0);
        }
        int status = 0;
        ASSERT_EQ(waitpid(pid, &status, 0), pid);
        ASSERT_TRUE(WIFEXITED(status)) << "the arm died on a signal";
        ASSERT_EQ(WEXITSTATUS(status), 0) << "see the arm's own failure text above";
    }
}

// The writer parks on the console, and the last window holder's exit reclaiming the device wakes
// it: its line goes out once, polled, after the reclaim.
TEST_F(ConsoleDark, the_reclaim_wakes_a_writer_parked_in_the_dark_window)
{
    run_isolated([]() {
        ASSERT_NO_FATAL_FAILURE(stage_dark());
        wake_next_park([](Thread* parked) {
            g_park_kind = parked->wait_kind;
            EXPECT_EQ(darkseam::g_wire, "") << "the line went out before the reclaim";
            darkseam::g_window_free = true;
            IrqLock lock;
            console_on_driver_death(lock);
        });
        EXPECT_EQ(kconsole_write_user("line\n", 5, true), 5);
        EXPECT_EQ(g_park_kind, WAIT_CONSOLE) << "the writer did not wait on the console";
        EXPECT_EQ(darkseam::g_wire, "line\n");
        EXPECT_EQ(darkseam::g_reclaims, 1u);
        EXPECT_EQ(g_writer->wait_kind, WAIT_NONE);
    });
}

// A restart's publish wakes it instead, and its line then belongs on the endpoint: the kernel
// console writes nothing and hands it back.
TEST_F(ConsoleDark, a_publish_wakes_a_writer_parked_in_the_dark_window)
{
    run_isolated([]() {
        ASSERT_NO_FATAL_FAILURE(stage_dark());
        wake_next_park([](Thread* parked) {
            g_park_kind = parked->wait_kind;
            {
                IrqLock lock;
                cap_console_serve(task(1), lock);
                console_handover_begin();
            }
            console_owner_set_user();
        });
        EXPECT_EQ(kconsole_write_user("line\n", 5, true), -KOS_EBUSY);
        EXPECT_EQ(g_park_kind, WAIT_CONSOLE);
        EXPECT_EQ(darkseam::g_wire, "");
        EXPECT_EQ(darkseam::g_reclaims, 0u);
    });
}
