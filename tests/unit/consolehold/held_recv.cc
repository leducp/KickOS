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
#include <kickos/sys/rights.h>

#include "ipc_seam.h"
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

    // A console is always published, as a console driver's create does; `published` says whether it
    // is the endpoint the driver serves.
    void stage(Console* c, bool published)
    {
        c->driver = seat_pool(0, PRIO_DRIVER);
        c->sender = seat_pool(1, PRIO_CLIENT);
        c->driver->affinity = 1u;
        c->sender->affinity = 1u;
        attach_caps(c->driver, KICKOS_SEAM_CAP_WIDTH);
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

    int32_t serve(Console* c, uint32_t* badge = nullptr)
    {
        memset(c->driver_buf, 0, sizeof(c->driver_buf));
        kos_reply_recv_opts opts{};
        kos_reply_recv_opts_init(&opts, c->ep_cap, 0, KOS_NEVER);
        int32_t const n = endpoint_reply_recv(KOS_CAP_NONE, reinterpret_cast<uintptr_t>(c->driver_buf),
                                              kos_call_lens_pack(0, sizeof(c->driver_buf)),
                                              reinterpret_cast<uintptr_t>(&opts));
        if (badge != nullptr)
        {
            *badge = opts.info.badge;
        }
        return n;
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

// A driver parked receiving while the console's task had ended is handed the held record at the
// serve, ahead of the sender queued beside it.
TEST_F(ConsoleHeldRecv, a_serve_hands_a_driver_already_parked_the_held_record_first)
{
    Console c{};
    ASSERT_NO_FATAL_FAILURE(stage(&c, true));
    seed_held("REC\n");
    Thread* const w = seat_pool(2, PRIO_DRIVER);
    detach_ready(w);
    static char wbuf[8];
    memset(wbuf, 0, sizeof(wbuf));
    IrqLock lock;
    testfix::seat_blocked(w);
    w->ipc.buf = reinterpret_cast<uintptr_t>(wbuf);
    w->ipc.len = sizeof(wbuf);
    w->ipc.badge_out = 0;
    w->wait_result = WAIT_RESULT_POISON;
    w->wait_kind = WAIT_EP_RECV;
    w->wait_obj = c.ep;
    w->wait_queue = &c.ep->recv_waiters;
    c.ep->recv_waiters.push_back(&w->link);
    c.ep->console = EP_CONSOLE_ENDED;

    cap_console_serve(task(1), lock);

    EXPECT_EQ(w->wait_result, 4) << "the parked driver was handed nothing";
    EXPECT_EQ(std::string(wbuf, 4), "REC\n") << "a sender went ahead of the held record";
    EXPECT_EQ(c.sender->state, ThreadState::BLOCKED);
    EXPECT_EQ(g_held_off, 4u);
}

// A console driver that drops a plain send lacking `write` still prints the kernel's records.
TEST_F(ConsoleHeldRecv, a_held_record_arrives_badged_with_write)
{
    Console c{};
    ASSERT_NO_FATAL_FAILURE(stage(&c, true));
    seed_held("REC\n");
    uint32_t badge = KOS_BADGE_NONE;
    ASSERT_EQ(serve(&c, &badge), 4);
    EXPECT_EQ(badge, static_cast<uint32_t>(KOS_RIGHT_WRITE));
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

namespace
{
    uintptr_t g_unmapped_buf = 0;
    char g_dying_buf[8];
    char g_live_buf[8];

    // A receiver parked on `ep` as endpoint_recv_locked leaves it, on a thread not running.
    Thread* park_at(int slot, uint8_t prio, Endpoint* ep, char* buf, size_t len = 8)
    {
        Thread* const w = seat_pool(slot, prio);
        detach_ready(w);
        memset(buf, 0, 8);
        IrqLock lock;
        testfix::seat_blocked(w);
        w->ipc.buf = reinterpret_cast<uintptr_t>(buf);
        w->ipc.len = len;
        w->ipc.badge_out = 0;
        w->wait_result = WAIT_RESULT_POISON;
        w->wait_kind = WAIT_EP_RECV;
        w->wait_obj = ep;
        w->wait_queue = &ep->recv_waiters;
        ep->recv_waiters.push_back(&w->link);
        return w;
    }
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
        ASSERT_TRUE(endpoint_servable(c.ep));
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

// A record held while the driver waits, under a map that cannot store into the driver's buffer,
// reaches the driver through its own receive rather than as -KOS_EFAULT.
TEST_F(ConsoleHeldRecv, a_driver_the_loaded_map_cannot_store_into_takes_the_record_itself)
{
    Console c{};
    ASSERT_NO_FATAL_FAILURE(stage(&c, true));
    {
        IrqLock lock;
        c.sender->wait_queue->unlink(&c.sender->link);
        c.sender->wait_queue = nullptr;
    }
    g_unmapped_buf = reinterpret_cast<uintptr_t>(c.driver_buf);
    wake_next_park([](Thread*) {
        seed_held("REC\n");
        testfix::g_ipc_seam_refuse_store = g_unmapped_buf;
        IrqLock lock;
        cap_console_deliver(lock);
        testfix::g_ipc_seam_refuse_store = 0;
    });
    int32_t const n = serve(&c);
    ASSERT_EQ(n, 4) << "the driver was answered for a store its own map admits";
    EXPECT_EQ(got(c, n), "REC\n");
    EXPECT_EQ(g_held_off, 4u);
}

// A dying receiver ahead of the driver is passed over: the record goes to the live one.
TEST_F(ConsoleHeldRecv, a_held_record_passes_over_a_dying_receiver)
{
    Console c{};
    ASSERT_NO_FATAL_FAILURE(stage(&c, true));
    seed_held("REC\n");
    Thread* const dying = park_at(2, PRIO_DRIVER + 2, c.ep, g_dying_buf);
    dying->dying = true;
    Thread* const live = park_at(3, PRIO_DRIVER - 1, c.ep, g_live_buf);
    {
        IrqLock lock;
        cap_console_deliver(lock);
    }
    EXPECT_EQ(live->wait_result, 4) << "the record went to a dying receiver";
    EXPECT_EQ(std::string(g_live_buf, 4), "REC\n");
    EXPECT_EQ(dying->wait_kind, WAIT_EP_RECV);
    dying->dying = false;
}

// A receiver with no room ahead of the driver takes no held record, and the record still goes
// to the driver behind it.
TEST_F(ConsoleHeldRecv, a_held_record_passes_over_a_receiver_with_no_room)
{
    Console c{};
    ASSERT_NO_FATAL_FAILURE(stage(&c, true));
    seed_held("REC\n");
    Thread* const roomless = park_at(2, PRIO_DRIVER + 2, c.ep, g_dying_buf, 0);
    Thread* const live = park_at(3, PRIO_DRIVER - 1, c.ep, g_live_buf);
    {
        IrqLock lock;
        cap_console_deliver(lock);
    }
    EXPECT_EQ(live->wait_result, 4) << "the record stuck behind a receiver with no room";
    EXPECT_EQ(std::string(g_live_buf, 4), "REC\n");
    EXPECT_EQ(roomless->wait_kind, WAIT_EP_RECV);
}

// The same at a serve: with no sender queued, the held record passes over the receiver with no
// room.
TEST_F(ConsoleHeldRecv, a_serve_passes_a_held_record_over_a_receiver_with_no_room)
{
    Console c{};
    ASSERT_NO_FATAL_FAILURE(stage(&c, true));
    seed_held("REC\n");
    Thread* roomless = nullptr;
    Thread* live = nullptr;
    {
        IrqLock lock;
        c.sender->wait_queue->unlink(&c.sender->link);
        c.sender->wait_queue = nullptr;
    }
    roomless = park_at(2, PRIO_DRIVER + 2, c.ep, g_dying_buf, 0);
    live = park_at(3, PRIO_DRIVER - 1, c.ep, g_live_buf);
    {
        IrqLock lock;
        c.ep->console = EP_CONSOLE_ENDED;
        cap_console_serve(task(1), lock);
    }
    EXPECT_EQ(live->wait_result, 4) << "the record stuck behind a receiver with no room";
    EXPECT_EQ(std::string(g_live_buf, 4), "REC\n");
    EXPECT_EQ(roomless->wait_kind, WAIT_EP_RECV);
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
