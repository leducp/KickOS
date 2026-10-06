// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A thread of the task serving the published console holds no stdout: the REAL kernel/syscall/
// cap.cc seats none at its spawn and empties one a publish finds, and the REAL
// kernel/init/console.cc then drops its line at once instead of waiting.

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
#include "syscall_internal.h"

using namespace kickos;
using namespace kickos::testfix;

namespace
{
    constexpr uint8_t PRIO_MEMBER = 8;
    constexpr uint8_t PRIO_OTHER = 4;

    class ConsoleOwnTask : public KSeam
    {
    };

    bool g_member_parked = false;

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

    int console_handle(Endpoint* ep)
    {
        return kernel().endpoints.handle_for(kernel().endpoints.index_of(ep));
    }

    // `publisher` publishes `ep` for `served`, and the handover completes.
    void publish(Thread* publisher, Endpoint* ep, Task* served)
    {
        {
            IrqLock lock;
            ASSERT_TRUE(cap_console_publish(publisher, console_handle(ep)));
            cap_console_serve(served);
            console_handover_begin();
        }
        console_owner_set_user();
    }

    uint8_t stdout_type(Thread const* t)
    {
        return cap_slot(t->caps, KOS_CAP_STDOUT)->type;
    }
}

// The driver's thread is spawned after the publish, as every bring-up spawns it: its stdout stays
// empty, while a client spawned at the same point gets the console. Its print then finds no
// endpoint and the kernel console drops the line without waiting, as a driver-owned UART takes
// nothing from the kernel.
TEST_F(ConsoleOwnTask, a_driver_thread_spawned_after_the_publish_has_no_stdout_and_its_print_returns)
{
    run_isolated([]() {
        darkseam::reset();
        Thread* const member = seat_pool(0, PRIO_MEMBER);
        attach_caps(member, KICKOS_CAP_CHILD_WIDTH);
        Thread* const client = seat_pool(1, PRIO_OTHER);
        attach_caps(client, KICKOS_CAP_CHILD_WIDTH);
        Thread* const publisher = seat_pool(2, PRIO_OTHER);
        attach_caps(publisher, KICKOS_CAP_CHILD_WIDTH);
        Task* const served = task(0);
        join_task(member, served);
        {
            IrqLock lock;
            sched::reschedule();
        }
        ASSERT_EQ(sched::current(), member);
        ASSERT_NO_FATAL_FAILURE(publish(publisher, endpoint(), served));
        {
            IrqLock lock;
            cap_install_defaults(member);
            cap_install_defaults(client);
        }
        EXPECT_EQ(stdout_type(client), static_cast<uint8_t>(CapType::CAP_ENDPOINT))
            << "fixture: a client spawned after the publish did not get the console";
        EXPECT_EQ(stdout_type(member), static_cast<uint8_t>(CapType::CAP_EMPTY))
            << "the console's own task got the console as its stdout";
        char const line[] = "line\n";
        EXPECT_EQ(endpoint_send(KOS_CAP_STDOUT, reinterpret_cast<uintptr_t>(line), 5,
                                KOS_TIMEOUT_NONE),
                  -KOS_EBADF);
        g_member_parked = false;
        wake_next_park([](Thread*) { g_member_parked = true; });
        EXPECT_EQ(kconsole_write_user(line, 5, true), 5);
        EXPECT_FALSE(g_member_parked) << "the print waited on a console its own task owns";
        EXPECT_EQ(darkseam::g_wire, "") << "the kernel wrote at a UART the driver owns";
    });
}

// The publisher serves the console from its own task: the publish that seats its stdout on the
// console empties that seat again once its task is named.
TEST_F(ConsoleOwnTask, a_publish_for_the_callers_own_task_leaves_its_stdout_empty)
{
    run_isolated([]() {
        darkseam::reset();
        Thread* const publisher = seat_pool(0, PRIO_MEMBER);
        attach_caps(publisher, KICKOS_CAP_CHILD_WIDTH);
        Task* const served = task(0);
        join_task(publisher, served);
        Endpoint* const ep = endpoint();
        int const idx = kernel().endpoints.index_of(ep);
        uint32_t const refs_before = kernel().endpoint_refs[idx];
        ASSERT_NO_FATAL_FAILURE(publish(publisher, ep, served));
        EXPECT_EQ(stdout_type(publisher), static_cast<uint8_t>(CapType::CAP_EMPTY))
            << "the publisher's stdout names the console its own task serves";
        EXPECT_EQ(kernel().endpoint_refs[idx], refs_before + 1u)
            << "the emptied seat kept its reference";
    });
}
