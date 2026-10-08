// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The receiving end of a held kernel record, over the real kernel/syscall/syscall_ipc.cc: the
// published console's driver takes held records ahead of every queued sender, a record wider
// than its buffer arrives over consecutive receives with nothing in between, a receiver
// already parked is handed the record at once, and no other endpoint ever sees one.

#include <string.h>

#include <string>

#include <kickos/cap.h>
#include <kickos/endpoint.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/sched.h>
#include <kickos/thread.h>

#include <kickos/sys/abi.h>

#include "kseam_test.h"
#include "syscall_internal.h"

using namespace kickos;
using namespace kickos::testfix;

namespace
{
    constexpr uint8_t PRIO_DRIVER = 10;
    constexpr uint8_t PRIO_CLIENT = 8;

    class ConsoleHeldRecv : public KSeam
    {
    };

    struct Console
    {
        Thread* driver;
        Thread* sender;
        Endpoint* ep;
        uint32_t ep_cap;
        char driver_buf[8];
        char sender_buf[4];
    };

    void seed_held(char const* s)
    {
        size_t const n = strlen(s);
        memcpy(g_held, s, n);
        g_held_len = static_cast<uint32_t>(n);
        g_held_off = 0;
    }

    // A console is always published, as kos_console_publish does; `published` says whether it
    // is the endpoint the driver serves.
    void stage(Console* c, bool published)
    {
        c->driver = seat_pool(0, PRIO_DRIVER);
        c->sender = seat_pool(1, PRIO_CLIENT);
        c->driver->affinity = 1u;
        c->sender->affinity = 1u;
        attach_caps(c->driver, KICKOS_CAP_CHILD_WIDTH);
        detach_ready(c->sender);
        testfix::seat_blocked(c->sender);
        {
            IrqLock lock;
            sched::reschedule(nullptr, lock);
        }
        ASSERT_EQ(sched::current(), c->driver);
        c->ep = endpoint();
        memset(c->driver_buf, 0, sizeof(c->driver_buf));
        memcpy(c->sender_buf, "abcd", sizeof(c->sender_buf));
        IrqLock lock;
        int const handle = kernel().endpoints.handle_for(kernel().endpoints.index_of(c->ep));
        ASSERT_EQ(cap_install(c->driver, handle, CapType::CAP_ENDPOINT, CAP_WAIT, &c->ep_cap), 0);
        c->ep->recv_holders = 1; // the WAIT just installed, counted as endpoint_create counts it
        int published_handle = handle;
        if (not published)
        {
            Endpoint* const console = endpoint();
            published_handle = kernel().endpoints.handle_for(kernel().endpoints.index_of(console));
        }
        ASSERT_TRUE(cap_console_publish(c->driver, published_handle, lock));
        cap_console_serve(task(0), lock);
        c->sender->ipc.buf = reinterpret_cast<uintptr_t>(c->sender_buf);
        c->sender->ipc.len = sizeof(c->sender_buf);
        c->sender->call_state = CALL_NONE;
        c->sender->wait_result = WAIT_RESULT_POISON;
        c->sender->wait_kind = WAIT_EP_SEND;
        c->sender->wait_obj = c->ep;
        c->sender->wait_queue = &c->ep->send_waiters;
        c->ep->send_waiters.push_back(&c->sender->link);
    }

    int32_t serve(Console* c)
    {
        memset(c->driver_buf, 0, sizeof(c->driver_buf));
        kos_reply_recv_opts opts{};
        kos_reply_recv_opts_init(&opts, c->ep_cap, 0, KOS_TIMEOUT_NONE);
        return endpoint_reply_recv(KOS_CAP_NONE, reinterpret_cast<uintptr_t>(c->driver_buf),
                                   kos_call_lens_pack(0, sizeof(c->driver_buf)),
                                   reinterpret_cast<uintptr_t>(&opts));
    }

    std::string got(Console const& c, int32_t n)
    {
        return std::string(c.driver_buf, static_cast<size_t>(n));
    }
}

// A sender was queued before the record: the record still reaches the driver first, so it
// lands between two sends rather than inside the driver's output of one.
TEST_F(ConsoleHeldRecv, the_driver_takes_a_held_record_ahead_of_a_queued_sender)
{
    Console c{};
    ASSERT_NO_FATAL_FAILURE(stage(&c, true));
    seed_held("REC\n");
    int32_t const first = serve(&c);
    ASSERT_EQ(first, 4);
    EXPECT_EQ(got(c, first), "REC\n") << "a queued sender's bytes went ahead of the record";
    EXPECT_EQ(c.sender->state, ThreadState::BLOCKED) << "the sender was served first";
    EXPECT_EQ(g_held_off, 4u) << "the record was handed over but not taken";
    int32_t const second = serve(&c);
    ASSERT_EQ(second, 4);
    EXPECT_EQ(got(c, second), "abcd");
}

// Eight bytes of buffer and a twelve-byte record: the rest comes on the next receive, before
// the sender.
TEST_F(ConsoleHeldRecv, a_record_wider_than_the_buffer_arrives_with_nothing_inside_it)
{
    Console c{};
    ASSERT_NO_FATAL_FAILURE(stage(&c, true));
    seed_held("0123456789AB");
    int32_t const first = serve(&c);
    ASSERT_EQ(first, 8);
    EXPECT_EQ(got(c, first), "01234567");
    int32_t const second = serve(&c);
    ASSERT_EQ(second, 4);
    EXPECT_EQ(got(c, second), "89AB") << "a sender's bytes landed inside the record";
    int32_t const third = serve(&c);
    ASSERT_EQ(third, 4);
    EXPECT_EQ(got(c, third), "abcd");
}

// A record held while the driver waits is handed to it at once, the way a send would be.
TEST_F(ConsoleHeldRecv, a_parked_driver_is_handed_the_record_at_once)
{
    Console c{};
    ASSERT_NO_FATAL_FAILURE(stage(&c, true));
    {
        IrqLock lock;
        c.sender->wait_queue->unlink(&c.sender->link);
        c.sender->wait_queue = nullptr;
        ASSERT_TRUE(endpoint_receiving(c.ep));
    }
    wake_next_park([](Thread*) {
        seed_held("REC\n");
        IrqLock lock;
        cap_console_deliver(lock);
    });
    int32_t const n = serve(&c);
    ASSERT_EQ(n, 4) << "the parked driver was not handed the record";
    EXPECT_EQ(got(c, n), "REC\n");
    EXPECT_EQ(g_held_off, 4u);
}

// Kernel records go to the console's driver only; another server's receive never sees one.
TEST_F(ConsoleHeldRecv, another_endpoint_never_receives_a_held_record)
{
    Console c{};
    ASSERT_NO_FATAL_FAILURE(stage(&c, false));
    seed_held("REC\n");
    int32_t const n = serve(&c);
    ASSERT_EQ(n, 4);
    EXPECT_EQ(got(c, n), "abcd");
    EXPECT_EQ(g_held_off, 0u);
}
