// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A published console dies with its task, over the real kernel/syscall/syscall_ipc.cc,
// kernel/syscall/cap.cc, kernel/task/task.cc and kernel/thread/park.cc: a send parks on it while
// its task lives, receiver or not, until that task's end refuses it; a send of no deadline never
// parks; and a kernel console writer waits out the dark window until the reclaim or a publish
// wakes it, or its own cancellation does.

#include <string.h>

#include <kickos/cap.h>
#include <kickos/endpoint.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <kickos/sys/abi.h>

#include "kseam_test.h"
#include "syscall_internal.h"

using namespace kickos;
using namespace kickos::testfix;

namespace
{
    constexpr uint8_t PRIO_WRITER = 8;

    class ConsoleLive : public KSeam
    {
    };

    struct Live
    {
        Thread* writer;
        Endpoint* ep;
        uint32_t cap;
        Task* served;
        char buf[4];
    };

    Live g_live{};
    bool g_parked_on_send = false;
    uint8_t g_park_kind = WAIT_NONE;

    // The writer runs; the console is published for a task of its own, and nobody holds WAIT on
    // it, so it has no receiver.
    void stage(Live* l, bool serve)
    {
        g_parked_on_send = false;
        g_park_kind = WAIT_NONE;
        l->writer = seat_pool(0, PRIO_WRITER);
        l->writer->affinity = 1u;
        attach_caps(l->writer, KICKOS_CAP_CHILD_WIDTH);
        {
            IrqLock lock;
            sched::reschedule();
        }
        ASSERT_EQ(sched::current(), l->writer);
        l->ep = endpoint();
        l->served = task(0);
        memcpy(l->buf, "abcd", sizeof(l->buf));
        IrqLock lock;
        int const handle = kernel().endpoints.handle_for(kernel().endpoints.index_of(l->ep));
        ASSERT_EQ(cap_install(l->writer, handle, CapType::CAP_ENDPOINT, CAP_SIGNAL, &l->cap), 0);
        ASSERT_TRUE(cap_console_publish(l->writer, handle));
        if (serve)
        {
            cap_console_serve(l->served);
        }
        l->ep->vacated = 1;
        ASSERT_FALSE(endpoint_receiving(l->ep));
    }

    int32_t send(Live const& l, uint32_t timeout_us)
    {
        return endpoint_send(l.cap, reinterpret_cast<uintptr_t>(l.buf), sizeof(l.buf), timeout_us);
    }
}

// The send parks although nobody receives, and only the task's end answers it.
TEST_F(ConsoleLive, a_send_parks_on_a_live_console_with_no_receiver_until_its_task_ends)
{
    ASSERT_NO_FATAL_FAILURE(stage(&g_live, true));
    wake_next_park([](Thread* parked) {
        g_parked_on_send = parked->wait_kind == WAIT_EP_SEND;
        EXPECT_EQ(g_console_noted, 0u) << "the console died before its task ended";
        IrqLock lock;
        task_end(g_live.served, 0, true);
    });
    int32_t const rc = send(g_live, KOS_TIMEOUT_NONE);
    EXPECT_TRUE(g_parked_on_send) << "the send was refused rather than parked";
    EXPECT_EQ(rc, -KOS_ECONNREFUSED) << "the task's end did not answer the parked sender";
    EXPECT_EQ(g_console_noted, 1u);
}

// The control: with no task serving it, the same console answers at once.
TEST_F(ConsoleLive, a_console_no_task_serves_refuses_a_send_at_once)
{
    ASSERT_NO_FATAL_FAILURE(stage(&g_live, false));
    EXPECT_EQ(send(g_live, KOS_TIMEOUT_NONE), -KOS_ECONNREFUSED);
}

// A send of no deadline never parks: a park here would find no waker and fail the fixture.
TEST_F(ConsoleLive, a_send_of_no_deadline_never_parks)
{
    ASSERT_NO_FATAL_FAILURE(stage(&g_live, true));
    uint32_t const parked = g_parked;
    EXPECT_EQ(send(g_live, 0u), SEND_WOULD_PARK);
    EXPECT_EQ(g_parked, parked);
}

// Outside the dark window the wait returns at once, with no park.
TEST_F(ConsoleLive, a_console_that_is_not_dark_is_not_waited_on)
{
    ASSERT_NO_FATAL_FAILURE(stage(&g_live, true));
    EXPECT_EQ(console_dark_wait(), 0);
}

// The writer waits on no deadline and the wake ends the wait.
TEST_F(ConsoleLive, a_dark_window_writer_waits_until_the_wake)
{
    ASSERT_NO_FATAL_FAILURE(stage(&g_live, true));
    g_console_dark = true;
    wake_next_park([](Thread* parked) {
        g_park_kind = parked->wait_kind;
        EXPECT_EQ(kernel().sleepq, nullptr) << "the dark window's wait is bounded";
        g_console_dark = false;
        IrqLock lock;
        console_dark_wake();
    });
    EXPECT_EQ(console_dark_wait(), 0) << "the wake did not end the wait";
    EXPECT_EQ(g_park_kind, WAIT_CONSOLE);
    EXPECT_EQ(g_live.writer->wait_kind, WAIT_NONE);
}

// A writer killed in the wait leaves it, told it was cancelled.
TEST_F(ConsoleLive, a_killed_writer_leaves_the_dark_window_wait)
{
    ASSERT_NO_FATAL_FAILURE(stage(&g_live, true));
    g_console_dark = true;
    wake_next_park([](Thread* parked) {
        g_park_kind = parked->wait_kind;
        IrqLock lock;
        thread_cancel_kind(parked, CANCEL_KILL);
    });
    EXPECT_EQ(console_dark_wait(), -KOS_ECANCELED);
    EXPECT_EQ(g_park_kind, WAIT_CONSOLE);
    EXPECT_EQ(g_live.writer->wait_kind, WAIT_NONE);
}

// A writer already cancelled never parks.
TEST_F(ConsoleLive, a_cancelled_writer_does_not_wait)
{
    ASSERT_NO_FATAL_FAILURE(stage(&g_live, true));
    g_console_dark = true;
    {
        IrqLock lock;
        (void)thread_cancel_escalate(g_live.writer, CANCEL_KILL);
    }
    EXPECT_EQ(console_dark_wait(), -KOS_ECANCELED);
}

namespace
{
    Thread* g_receiver = nullptr;
    char g_receiver_buf[8];

    // A receiver parked on the console that its task's end does not reach: one of that task not
    // slain yet, or one of another task.
    void park_receiver(Live* l)
    {
        g_receiver = seat_pool(1, PRIO_WRITER - 1);
        detach_ready(g_receiver);
        memset(g_receiver_buf, 0, sizeof(g_receiver_buf));
        IrqLock lock;
        g_receiver->state = ThreadState::BLOCKED;
        g_receiver->ipc.buf = reinterpret_cast<uintptr_t>(g_receiver_buf);
        g_receiver->ipc.len = sizeof(g_receiver_buf);
        g_receiver->ipc.badge_out = 0;
        g_receiver->wait_result = WAIT_RESULT_POISON;
        g_receiver->wait_kind = WAIT_EP_RECV;
        g_receiver->wait_obj = l->ep;
        g_receiver->wait_queue = &l->ep->recv_waiters;
        l->ep->recv_waiters.push_back(&g_receiver->link);
        l->ep->recv_holders = 1;
        l->ep->vacated = 0;
    }
}

// The task's end stops the console taking sends at once, though a receiver of it still waits: a
// line handed to it would die with it. The writer is answered as on an endpoint with no receiver,
// and the receiver is handed nothing.
TEST_F(ConsoleLive, no_line_is_handed_to_a_receiver_once_the_task_ended)
{
    ASSERT_NO_FATAL_FAILURE(stage(&g_live, true));
    park_receiver(&g_live);
    ASSERT_TRUE(endpoint_receiving(g_live.ep));
    {
        IrqLock lock;
        task_end(g_live.served, 0, true);
    }
    EXPECT_EQ(send(g_live, KOS_TIMEOUT_NONE), -KOS_EAGAIN);
    EXPECT_EQ(g_receiver->wait_kind, WAIT_EP_RECV) << "the line was handed to the receiver";
    EXPECT_EQ(g_receiver_buf[0], 0);
    IrqLock lock;
    EXPECT_FALSE(cap_console_serves(g_live.writer)) << "the writer is told to send there";
}

// The control: while the task lives, the same receiver takes the line.
TEST_F(ConsoleLive, a_receiver_of_a_live_console_takes_the_line)
{
    ASSERT_NO_FATAL_FAILURE(stage(&g_live, true));
    park_receiver(&g_live);
    EXPECT_EQ(send(g_live, KOS_TIMEOUT_NONE), 4);
    EXPECT_EQ(memcmp(g_receiver_buf, "abcd", 4), 0);
}

// O_NONBLOCK is its task's: the writer's task set it, so its send on the console never parks, and
// no other task's flag moved.
TEST_F(ConsoleLive, a_non_blocking_task_never_parks_on_the_console)
{
    ASSERT_NO_FATAL_FAILURE(stage(&g_live, true));
    Task* const mine = task(1);
    Task* const other = task(2);
    join_task(g_live.writer, mine);
    EXPECT_EQ(task_nonblock_call(KOS_NONBLOCK_SET), 1);
    EXPECT_TRUE(task_nonblocking(mine));
    EXPECT_FALSE(task_nonblocking(other)) << "a sibling task went non-blocking";
    uint32_t const parked = g_parked;
    EXPECT_EQ(send(g_live, KOS_TIMEOUT_NONE), SEND_WOULD_PARK);
    EXPECT_EQ(g_parked, parked);
    EXPECT_EQ(task_nonblock_call(KOS_NONBLOCK_GET), 1);
    EXPECT_EQ(task_nonblock_call(KOS_NONBLOCK_CLEAR), 0);
    EXPECT_EQ(task_nonblock_call(3), -KOS_EINVAL);
}

// The control: cleared, the same send parks again, until the task's end answers it.
TEST_F(ConsoleLive, a_blocking_task_parks_on_the_console)
{
    ASSERT_NO_FATAL_FAILURE(stage(&g_live, true));
    join_task(g_live.writer, task(1));
    wake_next_park([](Thread* parked) {
        g_parked_on_send = parked->wait_kind == WAIT_EP_SEND;
        IrqLock lock;
        task_end(g_live.served, 0, true);
    });
    EXPECT_EQ(send(g_live, KOS_TIMEOUT_NONE), -KOS_ECONNREFUSED);
    EXPECT_TRUE(g_parked_on_send);
}

// Only the console: a non-blocking task's send on an ordinary endpoint with no receiver waiting
// parks as it always did.
TEST_F(ConsoleLive, a_non_blocking_task_parks_on_another_endpoint)
{
    ASSERT_NO_FATAL_FAILURE(stage(&g_live, true));
    join_task(g_live.writer, task(1));
    (void)task_nonblock_call(KOS_NONBLOCK_SET);
    Endpoint* const plain = endpoint();
    plain->recv_holders = 1;
    uint32_t cap = 0;
    {
        IrqLock lock;
        int const handle = kernel().endpoints.handle_for(kernel().endpoints.index_of(plain));
        ASSERT_EQ(cap_install(g_live.writer, handle, CapType::CAP_ENDPOINT, CAP_SIGNAL, &cap), 0);
    }
    wake_next_park([](Thread* parked) {
        g_parked_on_send = parked->wait_kind == WAIT_EP_SEND;
        IrqLock lock;
        thread_abort_park(parked, -KOS_ETIMEDOUT);
    });
    g_parked_on_send = false;
    EXPECT_EQ(endpoint_send(cap, reinterpret_cast<uintptr_t>(g_live.buf), sizeof(g_live.buf),
                            KOS_TIMEOUT_NONE),
              -KOS_ETIMEDOUT);
    EXPECT_TRUE(g_parked_on_send) << "a non-blocking task's ordinary send did not park";
}

// A timed send keeps its own timeout for a non-blocking task: only a send of no timeout is
// answered at once.
TEST_F(ConsoleLive, a_non_blocking_task_keeps_a_timed_sends_timeout)
{
    ASSERT_NO_FATAL_FAILURE(stage(&g_live, true));
    join_task(g_live.writer, task(1));
    (void)task_nonblock_call(KOS_NONBLOCK_SET);
    g_parked_on_send = false;
    wake_next_park([](Thread* parked) {
        g_parked_on_send = parked->wait_kind == WAIT_EP_SEND;
        IrqLock lock;
        thread_abort_park(parked, -KOS_ETIMEDOUT);
    });
    EXPECT_EQ(send(g_live, 100u), -KOS_ETIMEDOUT);
    EXPECT_TRUE(g_parked_on_send) << "a timed send was answered before its timeout";
}

// A thread of no task has no flag to set or read.
TEST_F(ConsoleLive, a_thread_of_no_task_is_refused_the_flag)
{
    ASSERT_NO_FATAL_FAILURE(stage(&g_live, true));
    g_live.writer->task = nullptr;
    EXPECT_EQ(task_nonblock_call(KOS_NONBLOCK_GET), -KOS_EINVAL);
    EXPECT_EQ(task_nonblock_call(KOS_NONBLOCK_SET), -KOS_EINVAL);
}

namespace
{
    int g_next_console = -1;
}

// The console published again onto another endpoint: a writer parked on the old one is answered
// -KOS_EAGAIN, and its stdout names the new one, where it offers its line again.
TEST_F(ConsoleLive, a_writer_parked_on_a_replaced_console_is_answered_and_follows_it)
{
    ASSERT_NO_FATAL_FAILURE(stage(&g_live, true));
    g_parked_on_send = false;
    wake_next_park([](Thread* parked) {
        g_parked_on_send = parked->wait_kind == WAIT_EP_SEND;
        Thread* const publisher = seat_pool(1, PRIO_WRITER - 1);
        detach_ready(publisher);
        attach_caps(publisher, KICKOS_CAP_CHILD_WIDTH);
        Endpoint* const next = endpoint();
        IrqLock lock;
        g_next_console = kernel().endpoints.handle_for(kernel().endpoints.index_of(next));
        ASSERT_TRUE(cap_console_publish(publisher, g_next_console));
        cap_console_serve(g_live.served);
    });
    int32_t const rc = endpoint_send(KOS_CAP_STDOUT, reinterpret_cast<uintptr_t>(g_live.buf),
                                     sizeof(g_live.buf), KOS_TIMEOUT_NONE);
    EXPECT_TRUE(g_parked_on_send);
    EXPECT_EQ(rc, -KOS_EAGAIN) << "the writer was not answered, or told the seat is gone";
    EXPECT_EQ(cap_slot(g_live.writer->caps, KOS_CAP_STDOUT)->obj, g_next_console)
        << "the writer's stdout still names the replaced console";
    IrqLock lock;
    EXPECT_TRUE(cap_console_serves(g_live.writer));
}
