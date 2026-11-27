// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A user write once the console is published, end to end: the REAL kernel/init/console.cc
// refusing it, over the K-seam with nothing of the console stubbed but the device, while the
// driver lives, between its task's end and the reclaim, and once the kernel took the device back.

#include <string.h>

#include <stdio.h>

#include <string>

#include <kickos/cap.h>
#include <kickos/console_tx.h>
#include <kickos/diag.h>
#include <kickos/endpoint.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <kickos/sys/errno.h>

#include "device_seam.h"
#include "isolated_arm.h"
#include "kseam_test.h"

using namespace kickos;
using namespace kickos::testfix;

namespace
{
    constexpr uint8_t PRIO_WRITER = 8;

    class ConsoleRefused : public KSeam
    {
    };

    Thread* g_writer = nullptr;
    Task* g_served = nullptr;
    bool g_writer_parked = false;

    // The writer runs, its stdout naming the console, published for a task of its own whose
    // thread holds the register window.
    void stage_published()
    {
        deviceseam::reset();
        g_writer_parked = false;
        g_writer = seat_pool(0, PRIO_WRITER);
        attach_caps(g_writer, KICKOS_SEAM_CAP_WIDTH);
        {
            IrqLock lock;
            sched::reschedule(nullptr, lock);
        }
        ASSERT_EQ(sched::current(), g_writer);
        g_served = task(0);
        ASSERT_TRUE(deviceseam::publish(g_writer, endpoint(), g_served));
        deviceseam::g_window_free = false;
    }

    void end_served_task()
    {
        IrqLock lock;
        task_end(g_served, 0, true, lock);
    }

    // A write that would park records it and ends the park, so a wait fails the arm rather than
    // the fixture.
    int write_user(char const* line)
    {
        wake_next_park([](Thread*) { g_writer_parked = true; });
        return kconsole_write_user(line, strlen(line), true);
    }
}

// While the driver lives, a user write is refused at once and the device is not touched.
TEST_F(ConsoleRefused, a_user_write_to_a_live_published_console_is_refused)
{
    run_isolated([]() {
        ASSERT_NO_FATAL_FAILURE(stage_published());
        EXPECT_EQ(write_user("line\n"), -KOS_EBUSY);
        EXPECT_FALSE(g_writer_parked) << "the writer waited on a console it may not write";
        EXPECT_EQ(deviceseam::g_wire, "");
    });
}

// The driver's task ended while a thread of it still holds the device: the write is refused at
// once, with no wait for the reclaim, and the reclaim then lands with no user line behind it.
TEST_F(ConsoleRefused, a_user_write_between_the_drivers_death_and_the_reclaim_is_refused_at_once)
{
    run_isolated([]() {
        ASSERT_NO_FATAL_FAILURE(stage_published());
        end_served_task();
        ASSERT_EQ(deviceseam::g_reclaims, 0u) << "fixture: the reclaim did not wait for the window";
        EXPECT_EQ(write_user("line\n"), -KOS_EBUSY);
        EXPECT_FALSE(g_writer_parked) << "the writer waited for the reclaim";
        deviceseam::g_window_free = true;
        {
            IrqLock lock;
            console_on_driver_death();
        }
        EXPECT_EQ(deviceseam::g_reclaims, 1u);
        EXPECT_EQ(deviceseam::g_wire, diag::kConsoleReclaimed)
            << "a user line reached the reclaimed device";
    });
}

// Once reclaimed the device is the kernel's alone: a kernel line reaches it, a user line does not,
// blocking or not.
TEST_F(ConsoleRefused, the_reclaimed_console_takes_kernel_lines_and_refuses_user_lines)
{
    run_isolated([]() {
        ASSERT_NO_FATAL_FAILURE(stage_published());
        deviceseam::g_window_free = true;
        end_served_task();
        ASSERT_EQ(deviceseam::g_reclaims, 1u) << "fixture: the console was not reclaimed";
        EXPECT_EQ(write_user("user\n"), -KOS_EBUSY);
        EXPECT_EQ(kconsole_write_user("user\n", 5, false), -KOS_EBUSY);
        EXPECT_FALSE(g_writer_parked);
        kputs("kernel\n");
        EXPECT_EQ(deviceseam::g_wire, std::string(diag::kConsoleReclaimed) + "kernel\n")
            << "the kernel's line on the driver's death did not open the reclaimed device";
    });
}

// A restart's publish hands the device to the new driver: the write is still refused, and it
// belongs on the endpoint the seat names.
TEST_F(ConsoleRefused, a_user_write_after_a_restarts_publish_is_refused)
{
    run_isolated([]() {
        ASSERT_NO_FATAL_FAILURE(stage_published());
        deviceseam::g_window_free = true;
        end_served_task();
        ASSERT_EQ(deviceseam::g_reclaims, 1u);
        Endpoint* const ep = cap_console_endpoint();
        ASSERT_NE(ep, nullptr);
        ASSERT_TRUE(deviceseam::publish(g_writer, ep, task(1)));
        EXPECT_EQ(write_user("line\n"), -KOS_EBUSY);
        EXPECT_FALSE(g_writer_parked);
        EXPECT_EQ(deviceseam::g_wire, diag::kConsoleReclaimed);
    });
}

// The control: before any publish the kernel console is the users', and takes the line.
TEST_F(ConsoleRefused, a_user_write_before_any_publish_is_written)
{
    run_isolated([]() {
        deviceseam::reset();
        g_writer = seat_pool(0, PRIO_WRITER);
        attach_caps(g_writer, KICKOS_SEAM_CAP_WIDTH);
        {
            IrqLock lock;
            sched::reschedule(nullptr, lock);
        }
        EXPECT_EQ(kconsole_write_user("line\n", 5, true), 5);
        EXPECT_EQ(deviceseam::g_wire, "line\n");
    });
}
