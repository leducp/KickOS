// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// thread_effective_prio is the LIVE authority over a donee's effective priority, and these
// arms are what forbids replacing it with a cheap read of the donee.
//
// A donor's own priority can RISE after it donated. The kernel forwards such a raise along a
// mutex-to-mutex chain only (mutex_lock's second pass walks wait_mutex()->owner), so a raise
// that reaches a donor across an IPC edge leaves nothing on the donee saying so: the donee's
// seated `prio` can sit at its `base_prio` while a donor above it waits. Each arm below builds
// that state and then asks the funnel, once per donor term.
//
// The raise is applied through the same two lines park.cc and endpoint_recv's bounce arms use
// (`sched::set_prio(t, thread_effective_prio(t))`), so no arm invents a priority the kernel
// would not have written itself.

#include <kickos/endpoint.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/thread.h>

#include "kseam_test.h"

using namespace kickos;
using namespace kickos::testfix;

namespace
{
    constexpr uint8_t PRIO_DONEE = 8;
    constexpr uint8_t PRIO_DONOR = 8;
    constexpr uint8_t PRIO_URGENT = 20;

    // BLOCKED before the detach at every park below: on_remove reads `state` to tell a park
    // from a set_prio re-seat.
    void park_reply_donor(Thread* server, Thread* caller)
    {
        caller->state = ThreadState::BLOCKED;
        kernel().policy->on_remove(caller);
        caller->call_state = CALL_REPLY_WAIT;
        caller->wait_result = WAIT_RESULT_POISON;
        reply_donor_park(server, caller); // seats the wait edge itself
    }

    // park_plain_sender's caller-side twin: CALL_SEND_WAIT is the whole difference, and it is
    // what the funnel counts.
    void park_call_sender(Thread* caller, Endpoint* ep)
    {
        caller->state = ThreadState::BLOCKED;
        kernel().policy->on_remove(caller);
        caller->wait_queue = &ep->send_waiters;
        caller->wait_kind = WAIT_EP_SEND;
        caller->wait_obj = ep;
        caller->call_state = CALL_SEND_WAIT;
        caller->wait_result = WAIT_RESULT_POISON;
        ep->send_waiters.push_back(&caller->link);
    }

    // `donor` is already parked on its own donee. It serves an endpoint of its own, an urgent
    // caller queues on it, and the recompute seats that boost. Returns with donor->prio raised
    // and donor->base_prio untouched.
    void raise_donor_through_its_own_service(Thread* donor, int urgent_slot)
    {
        Endpoint* const served = endpoint();
        endpoint_server_set(served, donor);
        Thread* const urgent = spawn(urgent_slot, PRIO_URGENT);
        park_call_sender(urgent, served);
        {
            IrqLock lock;
            sched::set_prio(donor, thread_effective_prio(donor));
        }
    }
}

class PrioDonor : public kickos::testfix::KSeam
{
};

TEST_F(PrioDonor, a_reply_donor_raised_after_it_donated_still_reaches_the_server)
{
    IrqLock lock;
    Thread* const server = spawn(0, PRIO_DONEE);
    Thread* const caller = spawn(1, PRIO_DONOR);
    park_reply_donor(server, caller);
    EXPECT_EQ(thread_effective_prio(server), PRIO_DONEE)
        << "a donor at the server's own priority moves nothing";

    raise_donor_through_its_own_service(caller, 2);

    EXPECT_EQ(caller->prio, PRIO_URGENT) << "the donor took the urgent caller's priority";
    EXPECT_EQ(caller->base_prio, PRIO_DONOR) << "a boost never moves the anchor";
    EXPECT_EQ(server->prio, server->base_prio)
        << "nothing is seated on the server, which is exactly the state a cheap pre-test reads";
    EXPECT_EQ(thread_effective_prio(server), PRIO_URGENT)
        << "the funnel must re-derive the donor's CURRENT priority";
}

TEST_F(PrioDonor, a_send_wait_donor_raised_after_it_queued_still_reaches_the_server)
{
    IrqLock lock;
    Thread* const server = spawn(0, PRIO_DONEE);
    Endpoint* const ep = endpoint();
    endpoint_server_set(ep, server);
    Thread* const caller = spawn(1, PRIO_DONOR);
    park_call_sender(caller, ep);
    EXPECT_EQ(thread_effective_prio(server), PRIO_DONEE);

    raise_donor_through_its_own_service(caller, 2);

    EXPECT_EQ(server->prio, server->base_prio);
    EXPECT_EQ(thread_effective_prio(server), PRIO_URGENT)
        << "a queued caller donates at the priority it holds NOW, not the one it queued at";
}

TEST_F(PrioDonor, a_mutex_waiter_raised_by_an_ipc_donation_still_reaches_the_owner)
{
    IrqLock lock;
    Thread* const owner = spawn(0, PRIO_DONEE);
    int handle = 0;
    Mutex* const m = own_mutex(owner, &handle);
    Thread* const waiter = spawn(1, PRIO_DONOR);
    park_mutex_waiter(waiter, m);
    EXPECT_EQ(thread_effective_prio(owner), PRIO_DONEE);

    // mutex_lock's chain walk is what would normally carry this raise to the owner; it is not
    // running here, the raise arriving on the waiter's IPC side.
    raise_donor_through_its_own_service(waiter, 2);

    EXPECT_EQ(owner->prio, owner->base_prio);
    EXPECT_EQ(thread_effective_prio(owner), PRIO_URGENT);
}

TEST_F(PrioDonor, the_funnel_falls_back_to_the_anchor_when_the_donor_unlinks)
{
    IrqLock lock;
    Thread* const server = spawn(0, PRIO_DONEE);
    Thread* const caller = spawn(1, PRIO_DONOR);
    park_reply_donor(server, caller);
    raise_donor_through_its_own_service(caller, 2);
    EXPECT_EQ(thread_effective_prio(server), PRIO_URGENT);

    EXPECT_TRUE(reply_donor_unpark(server, caller));
    caller->call_state = CALL_NONE;

    EXPECT_EQ(thread_effective_prio(server), PRIO_DONEE)
        << "the URGENT the arms above read came from this donor's membership and from nothing "
           "else in the fixture";
}
