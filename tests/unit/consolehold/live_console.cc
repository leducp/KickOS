// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A published console dies with its task, over the real kernel/syscall/syscall_ipc.cc,
// kernel/syscall/cap.cc, kernel/task/task.cc and kernel/thread/park.cc: a send parks on it while
// its task lives, receiver or not, until that task's end refuses it where nothing else can serve
// it; a send of no deadline never parks; and a publish onto another endpoint, of a live console or
// a dead one, moves every stdout seat naming it and every sender parked on it.

#include <string.h>

#include <kickos/aspace.h>
#include <kickos/cap.h>
#include <kickos/endpoint.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/notify.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <kickos/sys/abi.h>

#include "ipc_seam.h"
#include "kseam_test.h"
#include "syscall_internal.h"

using namespace kickos;
using namespace kickos::testfix;

static_assert(ARCH_USER_STORE_CHECKED == 1, "the K-seam posture leaves kernel stores unchecked");

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

    // The writer runs; the console is published for a task of its own, and nobody holds WAIT on
    // it, so it has no receiver.
    void stage(Live* l, bool serve)
    {
        g_parked_on_send = false;
        l->writer = seat_pool(0, PRIO_WRITER);
        l->writer->affinity = 1u;
        attach_caps(l->writer, KICKOS_SEAM_CAP_WIDTH);
        {
            IrqLock lock;
            sched::reschedule(nullptr, lock);
        }
        ASSERT_EQ(sched::current(), l->writer);
        l->ep = endpoint();
        l->served = task(0);
        memcpy(l->buf, "abcd", sizeof(l->buf));
        IrqLock lock;
        int const handle = kernel().endpoints.handle_for(kernel().endpoints.index_of(l->ep));
        ASSERT_EQ(cap_install(l->writer, handle, CapType::CAP_ENDPOINT, CAP_SIGNAL, &l->cap), 0);
        ASSERT_TRUE(cap_console_publish(l->writer, handle, lock));
        if (serve)
        {
            cap_console_serve(l->served, lock);
        }
        ASSERT_EQ(l->ep->recv_holders, 0u);
        ASSERT_EQ(l->ep->handout_holders, 0u);
    }

    int32_t send(Live const& l, uint64_t deadline)
    {
        return endpoint_send(l.cap, reinterpret_cast<uintptr_t>(l.buf), sizeof(l.buf), deadline);
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
        task_end(g_live.served, 0, true, lock);
    });
    int32_t const rc = send(g_live, KOS_LIMIT_NONE);
    EXPECT_TRUE(g_parked_on_send) << "the send was refused rather than parked";
    EXPECT_EQ(rc, -KOS_ECONNREFUSED) << "the task's end did not answer the parked sender";
    EXPECT_EQ(g_console_noted, 1u);
}

// The control: with no task serving it, the same console answers at once.
TEST_F(ConsoleLive, a_console_no_task_serves_refuses_a_send_at_once)
{
    ASSERT_NO_FATAL_FAILURE(stage(&g_live, false));
    EXPECT_EQ(send(g_live, KOS_LIMIT_NONE), -KOS_ECONNREFUSED);
}

// A send of no deadline never parks: a park here would find no waker and fail the fixture.
TEST_F(ConsoleLive, a_send_of_no_deadline_never_parks)
{
    ASSERT_NO_FATAL_FAILURE(stage(&g_live, true));
    uint32_t const parked = g_parked;
    EXPECT_EQ(send(g_live, 0u), -KOS_EAGAIN);
    EXPECT_EQ(g_parked, parked);
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
        testfix::seat_blocked(g_receiver);
        g_receiver->ipc.buf = reinterpret_cast<uintptr_t>(g_receiver_buf);
        g_receiver->ipc.len = sizeof(g_receiver_buf);
        g_receiver->ipc.badge_out = 0;
        g_receiver->wait_result = WAIT_RESULT_POISON;
        g_receiver->wait_kind = WAIT_EP_RECV;
        g_receiver->wait_obj = l->ep;
        g_receiver->wait_queue = &l->ep->recv_waiters;
        l->ep->recv_waiters.push_back(&g_receiver->link);
        l->ep->recv_holders = 1;
    }
}

// The task's end stops the console handing lines to a receiver that still waits: a line handed to
// it would die with it. Its WAIT still counts, so the writer waits for another rather than being
// refused, and the receiver is handed nothing.
TEST_F(ConsoleLive, no_line_is_handed_to_a_receiver_once_the_task_ended)
{
    ASSERT_NO_FATAL_FAILURE(stage(&g_live, true));
    park_receiver(&g_live);
    {
        IrqLock lock;
        task_end(g_live.served, 0, true, lock);
    }
    ASSERT_TRUE(endpoint_servable(g_live.ep));
    EXPECT_EQ(send(g_live, 0u), -KOS_EAGAIN);
    EXPECT_EQ(g_receiver->wait_kind, WAIT_EP_RECV) << "the line was handed to the receiver";
    EXPECT_EQ(g_receiver_buf[0], 0);
}

namespace
{
    // A writer parked sending `line` on `ep`, as send_once leaves it, on a thread not running.
    Thread* park_sender(int slot, uint8_t prio, Endpoint* ep, char const* line)
    {
        Thread* const s = seat_pool(slot, prio);
        detach_ready(s);
        IrqLock lock;
        testfix::seat_blocked(s);
        s->ipc.buf = reinterpret_cast<uintptr_t>(line);
        s->ipc.len = 4;
        s->ipc.badge = KCAP_BADGE_NONE;
        s->call_state = CALL_NONE;
        s->wait_result = WAIT_RESULT_POISON;
        s->wait_kind = WAIT_EP_SEND;
        s->wait_obj = ep;
        s->wait_queue = &ep->send_waiters;
        ep->send_waiters.push_back(&s->link);
        return s;
    }

    char const LINE_FIRST[] = "1st.";
    char const LINE_SECOND[] = "2nd.";

    // The console's task ended with its driver parked receiving and two writers parked beside it.
    void stage_ended_with_writers(uint8_t first_prio, uint8_t second_prio, Thread** first,
                                  Thread** second)
    {
        ASSERT_NO_FATAL_FAILURE(stage(&g_live, true));
        park_receiver(&g_live);
        {
            IrqLock lock;
            task_end(g_live.served, 0, true, lock);
        }
        *first = park_sender(2, first_prio, g_live.ep, LINE_FIRST);
        *second = park_sender(3, second_prio, g_live.ep, LINE_SECOND);
    }
}

// The serve hands the line to the driver under its own lock: no writer has to run for it, so a
// writer slain or cancelled afterwards strands nobody, and the other writer waits for the
// driver's next receive.
TEST_F(ConsoleLive, a_serve_hands_a_driver_parked_before_it_the_first_writers_line_at_once)
{
    Thread* first = nullptr;
    Thread* second = nullptr;
    ASSERT_NO_FATAL_FAILURE(stage_ended_with_writers(PRIO_WRITER + 1, PRIO_WRITER - 1, &first,
                                                     &second));
    {
        IrqLock lock;
        cap_console_serve(task(1), lock);
        EXPECT_EQ(first->wait_result, 4) << "the line waited for its writer to run";
        EXPECT_EQ(g_receiver->wait_result, 4) << "the driver waited for a writer to run";
    }
    EXPECT_EQ(memcmp(g_receiver_buf, LINE_FIRST, 4), 0);
    EXPECT_EQ(second->state, ThreadState::BLOCKED);
    EXPECT_EQ(second->wait_kind, WAIT_EP_SEND) << "the second writer left the queue";
}

// Writers of one priority reach the driver in the order they parked.
TEST_F(ConsoleLive, a_serve_keeps_the_order_of_writers_of_one_priority)
{
    Thread* first = nullptr;
    Thread* second = nullptr;
    ASSERT_NO_FATAL_FAILURE(stage_ended_with_writers(PRIO_WRITER, PRIO_WRITER, &first, &second));
    {
        IrqLock lock;
        cap_console_serve(task(1), lock);
    }
    EXPECT_EQ(memcmp(g_receiver_buf, LINE_FIRST, 4), 0) << "the later writer went first";
    EXPECT_EQ(second->wait_kind, WAIT_EP_SEND);
}

namespace
{
    struct Parked
    {
        char buf[8];
        kos_recv_info info;
    };
    Parked g_parked_bufs[4];

    // A receiver parked on `ep` as endpoint_recv_locked leaves it, on a thread not running;
    // `info` selects a receive with metadata.
    Thread* park_receiver_at(int slot, uint8_t prio, Endpoint* ep, Parked* at, bool info)
    {
        Thread* const r = seat_pool(slot, prio);
        detach_ready(r);
        memset(at, 0, sizeof(*at));
        IrqLock lock;
        testfix::seat_blocked(r);
        r->ipc.buf = reinterpret_cast<uintptr_t>(at->buf);
        r->ipc.len = sizeof(at->buf);
        r->ipc.badge_out = 0;
        if (info)
        {
            r->ipc.badge_out = reinterpret_cast<uintptr_t>(&at->info);
        }
        r->wait_result = WAIT_RESULT_POISON;
        r->wait_kind = WAIT_EP_RECV;
        r->wait_obj = ep;
        r->wait_queue = &ep->recv_waiters;
        ep->recv_waiters.push_back(&r->link);
        ep->recv_holders = static_cast<uint8_t>(ep->recv_holders + 1u);
        return r;
    }

    bool queued_on(List const& q, Thread const* t)
    {
        for (ListNode const* n = q.head; n != nullptr; n = n->next)
        {
            if (n == &t->link)
            {
                return true;
            }
        }
        return false;
    }

    char g_call_buf[4] = {'c', 'a', 'l', 'l'};

    // A caller parked on `ep` before any receiver took it, as call_once leaves it.
    Thread* park_caller(int slot, uint8_t prio, Endpoint* ep)
    {
        Thread* const s = seat_pool(slot, prio);
        detach_ready(s);
        IrqLock lock;
        testfix::seat_blocked(s);
        s->ipc.buf = reinterpret_cast<uintptr_t>(g_call_buf);
        s->ipc.len = sizeof(g_call_buf);
        s->ipc.badge = KCAP_BADGE_NONE;
        s->call_rx_cap = sizeof(g_call_buf);
        s->call_state = CALL_SEND_WAIT;
        s->wait_result = WAIT_RESULT_POISON;
        s->wait_kind = WAIT_EP_SEND;
        s->wait_obj = ep;
        s->wait_queue = &ep->send_waiters;
        ep->send_waiters.push_back(&s->link);
        return s;
    }

    // The console published and its task ended, nobody parked on it yet.
    void stage_ended()
    {
        ASSERT_NO_FATAL_FAILURE(stage(&g_live, true));
        IrqLock lock;
        task_end(g_live.served, 0, true, lock);
    }

    void serve_again()
    {
        IrqLock lock;
        cap_console_serve(task(1), lock);
    }
}

// A take that lowers its receiver's priority, refusing a call it boosted it for, still hands its
// line to that receiver and not to the one now ahead of it.
TEST_F(ConsoleLive, a_serve_hands_the_line_to_the_receiver_whose_take_it_was)
{
    ASSERT_NO_FATAL_FAILURE(stage_ended());
    Thread* const boosted = park_receiver_at(2, PRIO_WRITER - 3, g_live.ep, &g_parked_bufs[0], false);
    boosted->prio = PRIO_WRITER + 6;
    Thread* const other = park_receiver_at(3, PRIO_WRITER + 1, g_live.ep, &g_parked_bufs[1], false);
    Thread* const caller = park_caller(4, PRIO_WRITER + 4, g_live.ep);
    Thread* const writer = park_sender(5, PRIO_WRITER, g_live.ep, LINE_FIRST);

    serve_again();

    EXPECT_EQ(caller->wait_result, -KOS_ENOTSUP) << "a call reached a receive without metadata";
    EXPECT_EQ(writer->wait_result, 4);
    EXPECT_EQ(boosted->wait_result, 4) << "the receiver that took the line was not answered";
    EXPECT_EQ(memcmp(g_parked_bufs[0].buf, LINE_FIRST, 4), 0);
    EXPECT_EQ(other->wait_kind, WAIT_EP_RECV) << "another receiver was answered for the line";
}

// A dying receiver ahead of a live one does not stop the serve handing the line to the live one.
TEST_F(ConsoleLive, a_serve_passes_over_a_dying_receiver_to_a_live_one)
{
    ASSERT_NO_FATAL_FAILURE(stage_ended());
    Thread* const dying = park_receiver_at(2, PRIO_WRITER + 6, g_live.ep, &g_parked_bufs[0], false);
    dying->dying = true;
    Thread* const live = park_receiver_at(3, PRIO_WRITER - 1, g_live.ep, &g_parked_bufs[1], false);
    Thread* const writer = park_sender(4, PRIO_WRITER, g_live.ep, LINE_FIRST);

    serve_again();

    EXPECT_EQ(writer->wait_result, 4) << "the line stayed parked beside a live receiver";
    EXPECT_EQ(live->wait_result, 4);
    EXPECT_EQ(memcmp(g_parked_bufs[1].buf, LINE_FIRST, 4), 0);
    EXPECT_EQ(dying->wait_kind, WAIT_EP_RECV);
    dying->dying = false;
}

// A receiver with no room for a reply capability refuses the call -KOS_EMFILE and takes the next
// line, as a receive does.
TEST_F(ConsoleLive, a_serve_refuses_a_call_to_a_receiver_with_no_reply_room)
{
    ASSERT_NO_FATAL_FAILURE(stage_ended());
    Thread* const full = park_receiver_at(2, PRIO_WRITER, g_live.ep, &g_parked_bufs[0], true);
    Thread* const caller = park_caller(3, PRIO_WRITER + 2, g_live.ep);
    Thread* const writer = park_sender(4, PRIO_WRITER, g_live.ep, LINE_FIRST);

    serve_again();

    EXPECT_EQ(caller->wait_result, -KOS_EMFILE);
    EXPECT_EQ(writer->wait_result, 4);
    EXPECT_EQ(full->wait_result, 4);
    EXPECT_EQ(memcmp(g_parked_bufs[0].buf, LINE_FIRST, 4), 0);
}

// A caller taken by a receiver other than the endpoint's server leaves no boost on that server.
TEST_F(ConsoleLive, a_serve_settles_the_donation_a_taken_caller_gave_the_server)
{
    ASSERT_NO_FATAL_FAILURE(stage_ended());
    Thread* const taker = park_receiver_at(2, PRIO_WRITER, g_live.ep, &g_parked_bufs[0], true);
    attach_caps(taker, KICKOS_SEAM_CAP_WIDTH);
    Thread* const server = seat_pool(3, PRIO_WRITER - 5);
    detach_ready(server);
    Thread* const caller = park_caller(4, PRIO_WRITER + 6, g_live.ep);
    {
        IrqLock lock;
        testfix::seat_blocked(server);
        endpoint_server_set(g_live.ep, server);
        pi_raise(server, caller->prio, lock);
    }
    ASSERT_EQ(server->prio, caller->prio) << "fixture: the caller boosted the server";

    serve_again();

    EXPECT_EQ(caller->wait_kind, WAIT_EP_REPLY) << "the call was not taken";
    EXPECT_EQ(taker->wait_result, 4);
    EXPECT_EQ(server->prio, PRIO_WRITER - 5) << "the server kept the boost of a call it never took";
}

// A call refused to a receiver taking without metadata leaves no boost on the server, when the
// server is not that receiver.
TEST_F(ConsoleLive, a_serve_settles_the_donation_of_a_call_it_refused)
{
    ASSERT_NO_FATAL_FAILURE(stage_ended());
    Thread* const taker = park_receiver_at(2, PRIO_WRITER, g_live.ep, &g_parked_bufs[0], false);
    Thread* const server = seat_pool(3, PRIO_WRITER - 5);
    detach_ready(server);
    Thread* const caller = park_caller(4, PRIO_WRITER + 6, g_live.ep);
    {
        IrqLock lock;
        testfix::seat_blocked(server);
        endpoint_server_set(g_live.ep, server);
        pi_raise(server, caller->prio, lock);
    }
    ASSERT_EQ(server->prio, caller->prio) << "fixture: the caller boosted the server";

    serve_again();

    EXPECT_EQ(caller->wait_result, -KOS_ENOTSUP);
    EXPECT_EQ(taker->wait_kind, WAIT_EP_RECV) << "the receiver was answered with nothing taken";
    EXPECT_EQ(server->prio, PRIO_WRITER - 5) << "the server kept the boost of a refused call";
}

// A receiver whose buffer the loaded map cannot store into is woken to receive again, nothing
// taken for it, and the serve hands the line to the next receiver.
TEST_F(ConsoleLive, a_serve_wakes_a_receiver_it_cannot_store_into_and_hands_the_line_to_the_next)
{
    ASSERT_NO_FATAL_FAILURE(stage_ended());
    Thread* const unmapped = park_receiver_at(2, PRIO_WRITER + 2, g_live.ep, &g_parked_bufs[0],
                                              false);
    Thread* const mapped = park_receiver_at(3, PRIO_WRITER - 1, g_live.ep, &g_parked_bufs[1], false);
    Thread* const writer = park_sender(4, PRIO_WRITER, g_live.ep, LINE_FIRST);

    testfix::g_ipc_seam_refuse_store = reinterpret_cast<uintptr_t>(g_parked_bufs[0].buf);
    serve_again();
    testfix::g_ipc_seam_refuse_store = 0;

    EXPECT_EQ(unmapped->wait_result, RECV_AGAIN) << "the receiver stayed parked unanswered";
    EXPECT_FALSE(queued_on(g_live.ep->recv_waiters, unmapped));
    EXPECT_EQ(g_parked_bufs[0].buf[0], 0);
    EXPECT_EQ(writer->wait_result, 4) << "the line stayed parked beside a receiver it could reach";
    EXPECT_EQ(mapped->wait_result, 4);
    EXPECT_EQ(memcmp(g_parked_bufs[1].buf, LINE_FIRST, 4), 0);
}

namespace
{
    char g_own_buf[8];
    Thread* g_queued = nullptr;
    Thread* g_other = nullptr;
    uint32_t g_wait_cap = KCAP_INVALID;
    uint32_t g_note = KCAP_INVALID;
    kos_monotonic_t g_until = KOS_NEVER;
    uint32_t g_admit = 0;
    kos_reply_recv_opts g_opts;
    int32_t g_own_rc = 0;

    // The console ended, and its writer, holding WAIT on it, receives on it as its driver.
    void stage_own_receiver()
    {
        ASSERT_NO_FATAL_FAILURE(stage_ended());
        {
            IrqLock lock;
            int const handle = kernel().endpoints.handle_for(kernel().endpoints.index_of(g_live.ep));
            ASSERT_EQ(cap_install(g_live.writer, handle, CapType::CAP_ENDPOINT, CAP_WAIT,
                                  &g_wait_cap),
                      0);
        }
        g_live.ep->recv_holders = 1;
        memset(g_own_buf, 0, sizeof(g_own_buf));
        g_queued = nullptr;
        g_other = nullptr;
        g_until = KOS_NEVER;
        g_admit = 0;
        g_own_rc = 0;
    }

    // At the receiver's park: a writer parks, and the serve finds the receiver's buffer refused
    // by the map loaded.
    void serve_unmapped()
    {
        g_queued = park_sender(2, PRIO_WRITER, g_live.ep, LINE_FIRST);
        testfix::g_ipc_seam_refuse_store = reinterpret_cast<uintptr_t>(g_own_buf);
        serve_again();
        testfix::g_ipc_seam_refuse_store = 0;
    }

    void own_receive()
    {
        kos_reply_recv_opts_init(&g_opts, g_wait_cap, 0u, g_until);
        g_opts.notify = g_admit;
        g_own_rc = endpoint_reply_recv(KOS_CAP_NONE, reinterpret_cast<uintptr_t>(g_own_buf),
                                       kos_call_lens_pack(0, sizeof(g_own_buf)),
                                       reinterpret_cast<uintptr_t>(&g_opts));
    }
}

// The receiver so woken takes the line in its own receive, which never answers the wake itself.
TEST_F(ConsoleLive, a_receiver_woken_to_receive_again_takes_the_line_in_its_own_receive)
{
    ASSERT_NO_FATAL_FAILURE(stage_own_receiver());
    wake_next_park([](Thread*) { serve_unmapped(); });
    own_receive();
    ASSERT_NE(g_queued, nullptr) << "the receive never parked";
    EXPECT_EQ(g_own_rc, 4) << "the receive did not take the line itself";
    EXPECT_EQ(memcmp(g_own_buf, LINE_FIRST, 4), 0);
    EXPECT_EQ(g_queued->wait_result, 4);
}

// The receive again keeps the deadline the receive was given: passed by then, it times out
// rather than parking again.
TEST_F(ConsoleLive, a_receive_again_keeps_the_receives_deadline)
{
    ASSERT_NO_FATAL_FAILURE(stage_own_receiver());
    g_until.ns = static_cast<int64_t>(testfix::deadline_in(1000));
    wake_next_park([](Thread*) {
        g_other = park_receiver_at(3, PRIO_WRITER - 2, g_live.ep, &g_parked_bufs[1], false);
        testfix::g_now_ns += 2000;
        serve_unmapped();
    });
    own_receive();
    ASSERT_NE(g_other, nullptr) << "the receive never parked";
    EXPECT_EQ(g_own_rc, -KOS_ETIMEDOUT);
    EXPECT_EQ(g_other->wait_result, 4);
    EXPECT_EQ(g_own_buf[0], 0);
}

// A cancel raised while the receiver was woken ends it at the receive again; the line waits.
TEST_F(ConsoleLive, a_receive_again_heeds_a_cancel_raised_before_it)
{
    ASSERT_NO_FATAL_FAILURE(stage_own_receiver());
    wake_next_park([](Thread* parked) {
        serve_unmapped();
        IrqLock lock;
        thread_cancel_kind(parked, CANCEL_KILL, lock);
    });
    run_noreturn(own_receive);
    ASSERT_NE(g_queued, nullptr) << "the receive never parked";
    EXPECT_EQ(g_own_buf[0], 0) << "the cancelled receiver took the line";
    EXPECT_EQ(g_queued->wait_kind, WAIT_EP_SEND);
}

// A notification raised while the receiver was woken answers the receive again, as at entry.
TEST_F(ConsoleLive, a_receive_again_answers_a_notification_raised_before_it)
{
    ASSERT_NO_FATAL_FAILURE(stage_own_receiver());
    ASSERT_EQ(notify_create(g_live.writer, &g_note), 0);
    ASSERT_EQ(notify_bind(g_live.writer, g_note), 0);
    g_admit = 1u;
    wake_next_park([](Thread* parked) {
        serve_unmapped();
        ASSERT_EQ(notify_signal(parked, g_note), 0);
    });
    own_receive();
    ASSERT_NE(g_queued, nullptr) << "the receive never parked";
    EXPECT_EQ(g_own_rc, -KOS_ENOTIFY);
    EXPECT_EQ(g_opts.notify, 1u);
    EXPECT_EQ(g_queued->wait_kind, WAIT_EP_SEND) << "the line was taken over a notification";
}

// A receive again whose own map refuses the store too answers -KOS_EFAULT, as a receive does;
// a park here would find no waker and fail the fixture.
TEST_F(ConsoleLive, a_receive_again_its_own_map_refuses_answers_efault)
{
    ASSERT_NO_FATAL_FAILURE(stage_own_receiver());
    wake_next_park([](Thread*) {
        serve_unmapped();
        testfix::g_ipc_seam_refuse_write = reinterpret_cast<uintptr_t>(&g_opts.info);
    });
    own_receive();
    testfix::g_ipc_seam_refuse_write = 0;
    ASSERT_NE(g_queued, nullptr) << "the receive never parked";
    EXPECT_EQ(g_own_rc, -KOS_EFAULT);
    EXPECT_EQ(g_queued->wait_result, -KOS_EFAULT);
}

// A driver parked receiving while the console's task had ended takes nothing, and a writer
// parked beside it then waits; the serve that names the driver's task pairs the two.
TEST_F(ConsoleLive, a_writer_parked_beside_a_driver_parked_before_the_serve_is_taken)
{
    ASSERT_NO_FATAL_FAILURE(stage(&g_live, true));
    park_receiver(&g_live);
    {
        IrqLock lock;
        task_end(g_live.served, 0, true, lock);
    }
    g_parked_on_send = false;
    wake_next_park([](Thread* parked) {
        g_parked_on_send = parked->wait_kind == WAIT_EP_SEND;
        IrqLock lock;
        cap_console_serve(task(1), lock);
    });
    EXPECT_EQ(send(g_live, KOS_LIMIT_NONE), 4) << "the writer and the driver stayed parked";
    EXPECT_TRUE(g_parked_on_send);
    EXPECT_EQ(memcmp(g_receiver_buf, "abcd", 4), 0);
}

// A writer moved onto a republished console where the new driver is already parked receiving is
// taken by it at once.
TEST_F(ConsoleLive, a_writer_moved_onto_a_console_whose_driver_already_waits_is_taken)
{
    ASSERT_NO_FATAL_FAILURE(stage(&g_live, true));
    g_parked_on_send = false;
    wake_next_park([](Thread* parked) {
        g_parked_on_send = parked->wait_kind == WAIT_EP_SEND;
        Thread* const publisher = seat_pool(2, PRIO_WRITER - 1);
        detach_ready(publisher);
        attach_caps(publisher, KICKOS_SEAM_CAP_WIDTH);
        Live next = g_live;
        next.ep = endpoint();
        park_receiver(&next);
        IrqLock lock;
        int const handle = kernel().endpoints.handle_for(kernel().endpoints.index_of(next.ep));
        ASSERT_TRUE(cap_console_publish(publisher, handle, lock));
        cap_console_serve(g_live.served, lock);
    });
    int32_t const rc = endpoint_send(KOS_CAP_STDOUT, reinterpret_cast<uintptr_t>(g_live.buf),
                                     sizeof(g_live.buf), KOS_LIMIT_NONE);
    EXPECT_TRUE(g_parked_on_send);
    EXPECT_EQ(rc, 4) << "the moved writer and the waiting driver stayed parked";
    EXPECT_EQ(memcmp(g_receiver_buf, "abcd", 4), 0);
}

// The control: while the task lives, the same receiver takes the line.
TEST_F(ConsoleLive, a_receiver_of_a_live_console_takes_the_line)
{
    ASSERT_NO_FATAL_FAILURE(stage(&g_live, true));
    park_receiver(&g_live);
    EXPECT_EQ(send(g_live, KOS_LIMIT_NONE), 4);
    EXPECT_EQ(memcmp(g_receiver_buf, "abcd", 4), 0);
}

// A send with a passed deadline, the libc of a non-blocking task, never parks on the console.
TEST_F(ConsoleLive, a_send_with_a_passed_deadline_never_parks_on_the_console)
{
    ASSERT_NO_FATAL_FAILURE(stage(&g_live, true));
    uint32_t const parked = g_parked;
    EXPECT_EQ(send(g_live, 0u), -KOS_EAGAIN);
    EXPECT_EQ(g_parked, parked);
}

// The control: a send with no deadline parks, until the task's end answers it.
TEST_F(ConsoleLive, a_send_with_no_deadline_parks_on_the_console)
{
    ASSERT_NO_FATAL_FAILURE(stage(&g_live, true));
    join_task(g_live.writer, task(1));
    wake_next_park([](Thread* parked) {
        g_parked_on_send = parked->wait_kind == WAIT_EP_SEND;
        IrqLock lock;
        task_end(g_live.served, 0, true, lock);
    });
    EXPECT_EQ(send(g_live, KOS_LIMIT_NONE), -KOS_ECONNREFUSED);
    EXPECT_TRUE(g_parked_on_send);
}

namespace
{
    int g_next_console = -1;
}

namespace
{
    Thread* g_next_receiver = nullptr;
    char g_next_buf[8];
    int32_t g_next_rc = 0;
    bool g_moved = false;
}

// The console published again onto another endpoint: a writer parked on the old one moves with
// its stdout and keeps waiting there, and a receiver of the new one takes its line.
TEST_F(ConsoleLive, a_writer_parked_on_a_replaced_console_moves_to_the_new_one)
{
    ASSERT_NO_FATAL_FAILURE(stage(&g_live, true));
    g_parked_on_send = false;
    g_moved = false;
    g_next_rc = 0;
    memset(g_next_buf, 0, sizeof(g_next_buf));
    wake_next_park([](Thread* parked) {
        g_parked_on_send = parked->wait_kind == WAIT_EP_SEND;
        Thread* const publisher = seat_pool(1, PRIO_WRITER - 1);
        detach_ready(publisher);
        attach_caps(publisher, KICKOS_SEAM_CAP_WIDTH);
        Endpoint* const next = endpoint();
        uint32_t wait_cap = KCAP_INVALID;
        {
            IrqLock lock;
            g_next_console = kernel().endpoints.handle_for(kernel().endpoints.index_of(next));
            ASSERT_TRUE(cap_console_publish(publisher, g_next_console, lock));
            cap_console_serve(g_live.served, lock);
            g_moved = parked->wait_kind == WAIT_EP_SEND and parked->wait_obj == next
                      and next->send_waiters.head == &parked->link;
            g_next_receiver = seat_pool(2, PRIO_WRITER + 1);
            attach_caps(g_next_receiver, KICKOS_SEAM_CAP_WIDTH);
            ASSERT_TRUE(obj_ref_inc(CapType::CAP_ENDPOINT, g_next_console, CAP_WAIT));
            ASSERT_EQ(cap_install(g_next_receiver, g_next_console, CapType::CAP_ENDPOINT, CAP_WAIT,
                                  &wait_cap),
                      0);
            sched::reschedule(nullptr, lock);
        }
        ASSERT_EQ(sched::current(), g_next_receiver);
        kos_reply_recv_opts opts{};
        kos_reply_recv_opts_init(&opts, wait_cap, KOS_RECV_NO_INFO, KOS_NEVER);
        g_next_rc = endpoint_reply_recv(KOS_CAP_NONE, reinterpret_cast<uintptr_t>(g_next_buf),
                                        kos_call_lens_pack(0, sizeof(g_next_buf)),
                                        reinterpret_cast<uintptr_t>(&opts));
    });
    int32_t const rc = endpoint_send(KOS_CAP_STDOUT, reinterpret_cast<uintptr_t>(g_live.buf),
                                     sizeof(g_live.buf), KOS_LIMIT_NONE);
    EXPECT_TRUE(g_parked_on_send);
    EXPECT_TRUE(g_moved) << "the parked writer did not move to the new console";
    EXPECT_EQ(rc, 4) << "the writer was not served by the new console";
    EXPECT_EQ(g_next_rc, 4);
    EXPECT_EQ(memcmp(g_next_buf, "abcd", 4), 0);
    EXPECT_EQ(cap_slot(g_live.writer->caps, KOS_CAP_STDOUT)->obj, g_next_console)
        << "the writer's stdout still names the replaced console";
    EXPECT_EQ(cap_console_endpoint()->console, EP_CONSOLE_SERVED);
}

// A restart after the console's death publishes onto another endpoint: the seat naming the dead
// console follows it, and the next send is taken there.
TEST_F(ConsoleLive, a_stdout_seat_on_a_dead_console_follows_the_restarts_publish)
{
    ASSERT_NO_FATAL_FAILURE(stage(&g_live, true));
    {
        IrqLock lock;
        task_end(g_live.served, 0, true, lock);
    }
    EXPECT_EQ(endpoint_send(KOS_CAP_STDOUT, reinterpret_cast<uintptr_t>(g_live.buf),
                            sizeof(g_live.buf), KOS_LIMIT_NONE),
              -KOS_ECONNREFUSED)
        << "a send was taken by a console whose task ended";
    Thread* const publisher = seat_pool(2, PRIO_WRITER - 1);
    detach_ready(publisher);
    attach_caps(publisher, KICKOS_SEAM_CAP_WIDTH);
    Endpoint* const next = endpoint();
    int handle = -1;
    {
        IrqLock lock;
        handle = kernel().endpoints.handle_for(kernel().endpoints.index_of(next));
        ASSERT_TRUE(cap_console_publish(publisher, handle, lock));
        cap_console_serve(task(1), lock);
    }
    EXPECT_EQ(cap_slot(g_live.writer->caps, KOS_CAP_STDOUT)->obj, handle)
        << "the writer's stdout still names the dead console";
    g_receiver = nullptr;
    g_live.ep = next;
    park_receiver(&g_live);
    EXPECT_EQ(endpoint_send(KOS_CAP_STDOUT, reinterpret_cast<uintptr_t>(g_live.buf),
                            sizeof(g_live.buf), KOS_LIMIT_NONE),
              4)
        << "the restarted console did not take the send";
    EXPECT_EQ(memcmp(g_receiver_buf, "abcd", 4), 0);
}

// The first publish seats every live thread whose standard output is empty, as a later one moves
// the seats naming the console it replaces: a thread spawned before it writes to the service.
TEST_F(ConsoleLive, a_thread_spawned_before_the_first_publish_writes_to_the_service)
{
    Thread* const early = seat_pool(0, PRIO_WRITER);
    attach_caps(early, KICKOS_SEAM_CAP_WIDTH);
    {
        IrqLock lock;
        sched::reschedule(nullptr, lock);
    }
    ASSERT_EQ(sched::current(), early);
    ASSERT_EQ(cap_slot(early->caps, KOS_CAP_STDOUT)->type, static_cast<uint8_t>(CapType::CAP_EMPTY))
        << "fixture: the early thread already has a standard output";
    Thread* const publisher = seat_pool(2, PRIO_WRITER - 1);
    detach_ready(publisher);
    attach_caps(publisher, KICKOS_SEAM_CAP_WIDTH);
    g_live.ep = endpoint();
    memcpy(g_live.buf, "abcd", sizeof(g_live.buf));
    {
        IrqLock lock;
        int const handle = kernel().endpoints.handle_for(kernel().endpoints.index_of(g_live.ep));
        ASSERT_TRUE(cap_console_publish(publisher, handle, lock));
        cap_console_serve(task(0), lock);
    }
    park_receiver(&g_live);
    EXPECT_EQ(endpoint_send(KOS_CAP_STDOUT, reinterpret_cast<uintptr_t>(g_live.buf),
                            sizeof(g_live.buf), KOS_LIMIT_NONE),
              4)
        << "a thread spawned before the first publish has no way to the service";
    EXPECT_EQ(memcmp(g_receiver_buf, "abcd", 4), 0);
}
