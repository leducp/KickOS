// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A thread waiting on a task's death lends its priority to that task's threads over the real IPC
// and scheduler at two cores: a call donating to such a waiter reaches them, and a member running
// on the other core is re-placed there, both ways.

#include <kickos/cap.h>
#include <kickos/endpoint.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <kickos/sys/errno.h>

#include <new>

#include "kseam_test.h"
#include "syscall_internal.h"

using namespace kickos;
using namespace kickos::testfix;

namespace
{
    class DeathLendIpc : public KSeam
    {
    };

    constexpr uint8_t PRIO_MEMBER = 4;
    constexpr uint8_t PRIO_SERVER = 6;
    constexpr uint8_t PRIO_CLIENT = 12;

    Thread* g_member = nullptr;
    uint8_t g_while_called = 0;

    // A member of task `t`, on the run list a death waiter lends to.
    Thread* member_of(Task* t, int slot, uint32_t affinity)
    {
        Thread* const m = seat_pool(slot, PRIO_MEMBER);
        m->affinity = affinity;
        attach_caps(m, KICKOS_SEAM_CAP_WIDTH);
        join_task(m, t);
        return m;
    }

    Thread* seat_peer_idle()
    {
        alignas(Thread) static unsigned char store[sizeof(Thread)];
        Thread* const t = new (store) Thread{};
        t->base_prio = KICKOS_PRIO_IDLE;
        t->prio = KICKOS_PRIO_IDLE;
        sched::add_idle(t, core_at(1));
        return t;
    }
}

// A client calls a server that waits on a task's death: its donation reaches the task's member,
// and goes when the call gives up.
TEST_F(DeathLendIpc, a_call_donating_to_a_server_waiting_on_a_death_reaches_the_dying_member)
{
    Thread* const client = seat_pool(0, PRIO_CLIENT);
    client->affinity = 1u;
    attach_caps(client, KICKOS_SEAM_CAP_WIDTH);
    {
        IrqLock lock;
        sched::reschedule(nullptr, lock);
    }
    ASSERT_EQ(sched::current(), client);
    Task* const dying = task(1);
    g_member = member_of(dying, 1, 1u);
    Thread* const server = seat_pool(2, PRIO_SERVER);
    server->affinity = 1u;
    Endpoint* const ep = endpoint();
    ep->recv_holders = 1;
    ep->handout_holders = 1;
    uint32_t cap = KCAP_INVALID;
    {
        IrqLock lock;
        park_death_waiter(server, dying);
        pi_settle(g_member, lock);
        endpoint_server_set(ep, server);
        int const handle = kernel().endpoints.handle_for(kernel().endpoints.index_of(ep));
        ASSERT_EQ(cap_install(client, handle, CapType::CAP_ENDPOINT, CAP_SIGNAL, &cap), 0);
    }
    ASSERT_EQ(g_member->prio, PRIO_SERVER) << "fixture: the waiting server lends nothing";
    g_while_called = 0;
    wake_next_park([](Thread* parked) {
        g_while_called = g_member->prio;
        IrqLock lock;
        thread_abort_park(parked, -KOS_ETIMEDOUT, lock);
    });
    static char buf[4];
    EXPECT_EQ(endpoint_call(cap, reinterpret_cast<uintptr_t>(buf), sizeof(buf), sizeof(buf), 1000u),
              -KOS_ETIMEDOUT);
    EXPECT_EQ(g_while_called, PRIO_CLIENT)
        << "the call's donation stopped at the server waiting on the death";
    EXPECT_EQ(g_member->prio, PRIO_SERVER) << "the member keeps a donation the call took back";
}

// The server a client calls is itself a lent member of a dying task: the call's donation stands
// over the lend once the client is queued.
TEST_F(DeathLendIpc, a_call_raises_a_lent_member_serving_it_and_the_lend_does_not_undo_it)
{
    Thread* const client = seat_pool(0, PRIO_CLIENT);
    client->affinity = 1u;
    attach_caps(client, KICKOS_SEAM_CAP_WIDTH);
    {
        IrqLock lock;
        sched::reschedule(nullptr, lock);
    }
    ASSERT_EQ(sched::current(), client);
    Task* const dying = task(1);
    g_member = member_of(dying, 1, 1u);
    Thread* const slayer = seat_pool(2, PRIO_SERVER);
    slayer->affinity = 1u;
    Endpoint* const ep = endpoint();
    ep->recv_holders = 1;
    ep->handout_holders = 1;
    uint32_t cap = KCAP_INVALID;
    {
        IrqLock lock;
        park_death_waiter(slayer, dying);
        pi_settle(g_member, lock);
        endpoint_server_set(ep, g_member);
        int const handle = kernel().endpoints.handle_for(kernel().endpoints.index_of(ep));
        ASSERT_EQ(cap_install(client, handle, CapType::CAP_ENDPOINT, CAP_SIGNAL, &cap), 0);
    }
    ASSERT_EQ(g_member->prio, PRIO_SERVER) << "fixture: the member is not lent to";
    g_while_called = 0;
    wake_next_park([](Thread* parked) {
        g_while_called = g_member->prio;
        IrqLock lock;
        thread_abort_park(parked, -KOS_ETIMEDOUT, lock);
    });
    static char buf[4];
    EXPECT_EQ(endpoint_call(cap, reinterpret_cast<uintptr_t>(buf), sizeof(buf), sizeof(buf), 1000u),
              -KOS_ETIMEDOUT);
    EXPECT_EQ(g_while_called, PRIO_CLIENT) << "the lend's recompute undid the call's donation";
    EXPECT_EQ(g_member->prio, PRIO_SERVER) << "the member keeps a donation the call took back";
}

// The member runs on the other core: the lend and its end both reach it there.
TEST_F(DeathLendIpc, a_member_running_on_the_other_core_is_lent_to_and_let_go_there)
{
    Thread* const waiter = seat_pool(0, PRIO_CLIENT);
    waiter->affinity = 1u;
    {
        IrqLock lock;
        sched::reschedule(nullptr, lock);
    }
    ASSERT_EQ(sched::current(), waiter);
    (void)seat_peer_idle();
    Task* const dying = task(1);
    g_member = member_of(dying, 1, 1u << 1);
    testfix::seat_running_on(g_member, core_at(1));
    ASSERT_EQ(kernel().current(core_at(1)), g_member) << "fixture: the member is not on core one";
    {
        IrqLock lock;
        ParkToken const ask = park_cancel_pending(waiter);
        (void)death_park(ask)(waiter, dying, lock);
        death_wait_lend(dying, lock);
    }
    EXPECT_EQ(g_member->prio, PRIO_CLIENT) << "the member on the other core was not lent to";
    EXPECT_NE(g_member->reseat_owed, 0u) << "core one was not asked to re-place it";
    dispatch_as(1);
    ASSERT_EQ(g_member->reseat_owed, 0u) << "fixture: core one's pass left the re-place owed";
    {
        IrqLock lock;
        thread_abort_park(waiter, -KOS_ETIMEDOUT, lock);
    }
    EXPECT_EQ(g_member->prio, PRIO_MEMBER) << "the member on the other core keeps the lend";
    EXPECT_NE(g_member->reseat_owed, 0u) << "core one was not asked to re-place it again";
    EXPECT_TRUE(kernel().death_waiters.empty());
}
