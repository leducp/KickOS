// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A thread readied toward a peer, at two kernel cores, over the real publish, flush and drain.
//
// The waker writes nothing into the peer's ready structure: the thread is HANDED and staged on the
// waker's ring toward the peer, the ring is published where the waker's lock span ends, and the
// peer links the thread in the dispatch that enters its scheduler, or at its start.

#include <kickos/arch/arch.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/klock.h>
#include <kickos/sched.h>
#include <kickos/thread.h>

#include "kseam_test.h"

using namespace kickos;
using namespace kickos::testfix;

namespace
{
    constexpr uint32_t CORE_ME = 0;
    constexpr uint32_t CORE_PEER = 1;

    // Fixture storage, so every pool slot is free for the threads under test.
    constexpr int SLOT_PEER_IDLE = 0;
    constexpr int SLOT_RUNNER = 1;

    constexpr uint8_t PRIO_RUNNER = 6;
    constexpr uint8_t PRIO_UNDER = 5;

    int owed_at(uint32_t core)
    {
        uint32_t const was = g_core;
        g_core = core;
        int const owed = kickos_kernel_core_resched_owed();
        g_core = was;
        return owed;
    }

    // klock.cc's sequence rows outlive reset(), so an arm starts from what it drains here.
    void settle()
    {
        uint32_t const was = g_core;
        for (uint32_t core = 0; core < KICKOS_KERNEL_CORES; core++)
        {
            g_core = core;
            (void)kickos_kernel_core_resched_take();
        }
        g_core = was;
    }

    void pass_as(uint32_t core)
    {
        uint32_t const was = g_core;
        g_core = core;
        {
            IrqLock lock;
            sched::reschedule();
        }
        g_core = was;
    }

    void wake_as(uint32_t core, Thread* t)
    {
        uint32_t const was = g_core;
        g_core = core;
        {
            IrqLock lock;
            sched::wake(t);
        }
        g_core = was;
    }

    Thread* add_peer_idle()
    {
        Thread* const t = &g_fx.t[SLOT_PEER_IDLE];
        t->base_prio = KICKOS_PRIO_IDLE;
        t->prio = KICKOS_PRIO_IDLE;
        t->id = static_cast<uint16_t>(SLOT_PEER_IDLE + 1);
        sched::add_idle(t, CORE_PEER);
        return t;
    }

    // A runner pinned to this core and, when `peer_started`, the peer started on its idle. Pinned:
    // it lives outside the pool, and no ring can name it.
    Thread* runner_and_peer(bool peer_started)
    {
        Thread* const runner = spawn(SLOT_RUNNER, PRIO_RUNNER);
        runner->affinity = 1u << CORE_ME;
        Thread* const peer_idle = add_peer_idle();
        {
            IrqLock lock;
            sched::reschedule();
        }
        if (peer_started)
        {
            seat_running_on(peer_idle, CORE_PEER);
        }
        settle();
        return runner;
    }

    // A pool thread parked on `on`, whose mask is `mask` from the park on.
    Thread* parked(int slot, uint8_t prio, Thread* on, uint32_t mask)
    {
        Thread* const t = seat_pool(slot, prio);
        park_join(t, on);
        t->affinity = mask;
        settle();
        return t;
    }

    bool linked_on(Thread const* t, uint32_t core)
    {
        for (ListNode const* n = kernel().ready[core][t->prio].head; n != nullptr; n = n->next)
        {
            if (n == &t->link)
            {
                return true;
            }
        }
        return false;
    }

    bool linked_anywhere(Thread const* t)
    {
        for (uint32_t core = 0; core < KICKOS_KERNEL_CORES; core++)
        {
            if (linked_on(t, core))
            {
                return true;
            }
        }
        return false;
    }

    // Entries written toward `to` and not yet drained, the unpublished ones included.
    uint32_t in_flight(uint32_t from, uint32_t to)
    {
        return kernel().sched_out[from].staged[to] - kernel().sched_in[to].tail[from].load();
    }
}

class SchedRing : public kickos::testfix::KSeam
{
};

TEST_F(SchedRing, a_handoff_is_linked_by_the_target_dispatch_and_by_no_other)
{
    Thread* const runner = runner_and_peer(true);
    Thread* const b = parked(2, PRIO_UNDER, runner, 1u << CORE_PEER);

    wake_as(CORE_ME, b);

    ASSERT_EQ(b->state, ThreadState::HANDED) << "the waker linked a thread bound for a peer";
    ASSERT_EQ(b->queue_core, CORE_PEER) << "fixture: the thread is bound for the peer";
    EXPECT_FALSE(linked_anywhere(b)) << "a handed thread sits on a ready structure";

    pass_as(CORE_ME);
    pass_as(CORE_PEER);

    EXPECT_EQ(b->state, ThreadState::HANDED)
        << "an ordinary pass linked the thread: only the dispatch that enters the scheduler "
           "drains, so a pinned same-core handoff reads no ring";
    EXPECT_FALSE(linked_anywhere(b)) << "an ordinary pass linked the thread";

    dispatch_as(CORE_PEER);

    EXPECT_TRUE(linked_on(b, CORE_PEER)) << "the target's dispatch did not link its handoff";
    EXPECT_EQ(kernel().current[CORE_PEER], b) << "the idle target did not take its handoff";
}

TEST_F(SchedRing, a_handoff_to_an_unstarted_core_is_linked_at_its_start)
{
    Thread* const runner = runner_and_peer(false);
    Thread* const b = parked(2, PRIO_UNDER, runner, 1u << CORE_PEER);

    wake_as(CORE_ME, b);

    ASSERT_EQ(b->state, ThreadState::HANDED) << "fixture: the thread is bound for the peer";
    ASSERT_EQ(kernel().current[CORE_PEER], nullptr) << "fixture: the peer has not started";

    dispatch_as(CORE_PEER);

    EXPECT_EQ(b->state, ThreadState::HANDED) << "a core still in bring-up drained its ring";

    g_core = CORE_PEER;
    sched::start();
    g_core = CORE_ME;

    EXPECT_EQ(kernel().current[CORE_PEER], b)
        << "the peer started without linking what was handed to it before it started, and no "
           "later dispatch is owed that would";
}

TEST_F(SchedRing, a_handoff_whose_mask_no_longer_names_its_target_is_handed_on)
{
    Thread* const runner = runner_and_peer(true);
    Thread* const b = parked(2, PRIO_UNDER, runner, KICKOS_CORE_SET_ALL);

    wake_as(CORE_ME, b);
    ASSERT_EQ(b->state, ThreadState::HANDED) << "fixture: the declined wake went to the idle peer";
    ASSERT_EQ(b->queue_core, CORE_PEER) << "fixture: the declined wake went to the idle peer";

    sched::set_affinity(b, 1u << CORE_ME);

    dispatch_as(CORE_PEER);

    EXPECT_FALSE(linked_on(b, CORE_PEER)) << "the target linked a thread its mask refuses";
    EXPECT_EQ(b->state, ThreadState::HANDED) << "the refused handoff was not handed on";
    EXPECT_EQ(b->queue_core, CORE_ME) << "the refused handoff went nowhere the mask names";
    EXPECT_NE(owed_at(CORE_ME), 0) << "the handoff was handed on to a core nobody told";

    dispatch_as(CORE_ME);

    EXPECT_TRUE(linked_on(b, CORE_ME)) << "the core the mask names never linked it";
}

TEST_F(SchedRing, every_thread_in_flight_at_once_does_not_fill_one_ring)
{
    Thread* const runner = runner_and_peer(true);
    Thread* threads[KICKOS_THREAD_SLOTS] = {};
    for (int s = 0; s < KICKOS_THREAD_SLOTS; s++)
    {
        threads[s] = parked(s, PRIO_UNDER, runner, 1u << CORE_PEER);
    }

    {
        IrqLock lock;
        for (Thread* t : threads)
        {
            ASSERT_TRUE(sched::wake_no_resched(t)) << "fixture: every thread was parked";
            sched::set_prio(t, PRIO_UNDER + 1u);
        }
        ASSERT_EQ(in_flight(CORE_ME, CORE_PEER), 2u * KICKOS_THREAD_SLOTS)
            << "fixture: every pool thread is on one ring, handed and reseated";
        EXPECT_LE(in_flight(CORE_ME, CORE_PEER), SCHED_RING_DEPTH) << "the ring overflowed";
    }

    for (int round = 0; round <= 2 * KICKOS_THREAD_SLOTS; round++)
    {
        dispatch_as(CORE_PEER);
    }

    EXPECT_EQ(in_flight(CORE_ME, CORE_PEER), 0u) << "entries were left on the ring";
    for (Thread const* t : threads)
    {
        EXPECT_NE(t->state, ThreadState::HANDED) << "an entry was lost to a wrap";
        EXPECT_EQ(t->queue_core, CORE_PEER) << "an entry was delivered to the wrong core";
        EXPECT_EQ(t->rq_prio, PRIO_UNDER + 1u) << "a thread was linked on a stale priority";
        EXPECT_EQ(t->reseat_owed, 0u) << "a request was left owed";
    }
}

TEST_F(SchedRing, a_staged_entry_is_invisible_until_the_span_ends)
{
    Thread* const runner = runner_and_peer(true);
    Thread* const b = parked(2, PRIO_UNDER, runner, 1u << CORE_PEER);
    uint32_t const head = kernel().sched_out[CORE_ME].head[CORE_PEER].load();

    {
        IrqLock lock;
        ASSERT_TRUE(sched::wake_no_resched(b)) << "fixture: the thread was parked";
        ASSERT_EQ(b->state, ThreadState::HANDED) << "fixture: the thread is bound for the peer";
        EXPECT_EQ(kernel().sched_out[CORE_ME].head[CORE_PEER].load(), head)
            << "the entry was published inside the span: a switch this span books would let "
               "the target link a thread whose frame is still live";
        EXPECT_EQ(owed_at(CORE_PEER), 0) << "the target was asked inside the span";
    }

    EXPECT_EQ(kernel().sched_out[CORE_ME].head[CORE_PEER].load(), head + 1u)
        << "the span ended and its entry was not published";
    EXPECT_NE(owed_at(CORE_PEER), 0) << "the entry was published to a core nobody told";
}

TEST_F(SchedRing, a_drain_past_its_budget_owes_itself_the_next_dispatch)
{
    Thread* const runner = runner_and_peer(true);
    constexpr int OVER = static_cast<int>(SCHED_DRAIN_BUDGET) + 1;
    Thread* threads[OVER] = {};
    for (int s = 0; s < OVER; s++)
    {
        threads[s] = parked(s + 2, PRIO_UNDER, runner, 1u << CORE_PEER);
    }
    {
        IrqLock lock;
        for (Thread* t : threads)
        {
            ASSERT_TRUE(sched::wake_no_resched(t)) << "fixture: every thread was parked";
        }
    }

    dispatch_as(CORE_PEER);

    ASSERT_EQ(in_flight(CORE_ME, CORE_PEER), 1u) << "the dispatch did not stop at its budget";
    EXPECT_NE(owed_at(CORE_PEER), 0)
        << "the dispatch left an entry on the ring and owed itself nothing, so the entry waits "
           "for whatever next raises that core";

    dispatch_as(CORE_PEER);

    EXPECT_EQ(in_flight(CORE_ME, CORE_PEER), 0u) << "the dispatch it owed did not drain the rest";
}

TEST_F(SchedRing, a_wake_placed_by_the_collector_while_handed_is_left_to_its_target)
{
    Thread* const runner = runner_and_peer(true);
    Thread* const b = parked(2, PRIO_UNDER, runner, 1u << CORE_PEER);

    {
        IrqLock lock;
        ASSERT_TRUE(sched::wake_no_resched(b)) << "fixture: the thread was parked";
        sched::place_ready(b);
    }

    EXPECT_EQ(b->state, ThreadState::HANDED) << "the collector placed a thread in flight";
    EXPECT_EQ(b->queue_core, CORE_PEER) << "the collector moved a thread in flight";
    EXPECT_EQ(in_flight(CORE_ME, CORE_PEER), 1u) << "the collector staged a second entry";
}

namespace
{
    constexpr uint8_t PRIO_HOG = 7;
    constexpr uint8_t PRIO_BOOST = 8;

    // A thread READY on the peer behind the peer's running hog, both above everything this core
    // could hand it, and this core's runner still running.
    struct OnPeer
    {
        Thread* runner;
        Thread* hog;
        Thread* t;
    };

    OnPeer ready_on_peer(uint8_t t_prio)
    {
        OnPeer o{};
        o.runner = runner_and_peer(true);
        o.hog = seat_pool(2, PRIO_HOG);
        seat_running_on(o.hog, CORE_PEER);
        o.hog->affinity = 1u << CORE_PEER;
        o.t = parked(3, t_prio, o.runner, 1u << CORE_PEER);
        wake_as(CORE_ME, o.t);
        dispatch_as(CORE_PEER);
        settle();
        return o;
    }

    // Counts re-seats: a list removal of a thread that stays READY or RUNNING.
    SchedPolicy g_counted{};
    uint32_t g_reseats = 0;

    void counted_on_remove(Thread* t)
    {
        if (t->state == ThreadState::READY or t->state == ThreadState::RUNNING)
        {
            g_reseats++;
        }
        sched::default_policy()->on_remove(t);
    }

    void set_prio_as(uint32_t core, Thread* t, uint8_t p)
    {
        uint32_t const was = g_core;
        g_core = core;
        {
            IrqLock lock;
            sched::set_prio(t, p);
        }
        g_core = was;
    }

    void set_affinity_as(uint32_t core, Thread* t, uint32_t mask)
    {
        uint32_t const was = g_core;
        g_core = core;
        sched::set_affinity(t, mask);
        g_core = was;
    }

    void slay_as(uint32_t core, Thread* t)
    {
        uint32_t const was = g_core;
        g_core = core;
        {
            IrqLock lock;
            thread_cancel_kind(t, CANCEL_SLAY);
        }
        g_core = was;
    }

    // Request #1 is made toward the peer, the peer moves the thread here before its dispatch
    // reads it, and this core links it at `first`: #1 is still unretired on the ring toward the
    // peer, so the request that follows is the one its slot byte keeps out of every ring.
    OnPeer moved_here_behind_a_request(uint8_t first)
    {
        OnPeer o = ready_on_peer(PRIO_UNDER);
        o.t->affinity = KICKOS_CORE_SET_ALL;
        set_prio_as(CORE_ME, o.t, first);
        set_affinity_as(CORE_PEER, o.t, 1u << CORE_ME);
        dispatch_as(CORE_ME);
        return o;
    }
}

TEST_F(SchedRing, a_boost_of_a_thread_ready_on_a_peer_is_seated_by_that_peers_dispatch)
{
    OnPeer o = ready_on_peer(PRIO_UNDER);
    ASSERT_TRUE(linked_on(o.t, CORE_PEER)) << "fixture: the thread waits on the peer";
    uint32_t const bitmap = kernel().ready_bitmap[CORE_PEER];

    set_prio_as(CORE_ME, o.t, PRIO_BOOST);

    EXPECT_EQ(o.t->prio, PRIO_BOOST) << "the request did not write the field";
    EXPECT_EQ(kernel().ready_bitmap[CORE_PEER], bitmap)
        << "a core re-seated a thread in a peer's ready structure";
    EXPECT_EQ(o.t->rq_prio, PRIO_UNDER) << "a core re-keyed a thread a peer holds";
    EXPECT_EQ(in_flight(CORE_ME, CORE_PEER), 1u) << "the peer was sent no request";
    EXPECT_NE(owed_at(CORE_PEER), 0) << "the peer was not asked for the dispatch that applies it";

    dispatch_as(CORE_PEER);

    EXPECT_EQ(o.t->rq_prio, PRIO_BOOST) << "the peer's dispatch did not re-seat the boost";
    EXPECT_EQ(o.t->reseat_owed, 0u) << "the applied request left its slot owed";
    EXPECT_EQ(kernel().current[CORE_PEER], o.t) << "the boosted thread does not run over the hog";
}

TEST_F(SchedRing, a_reseat_of_a_handed_thread_is_read_at_its_link)
{
    Thread* const runner = runner_and_peer(true);
    Thread* const b = parked(2, PRIO_UNDER, runner, 1u << CORE_PEER);
    {
        IrqLock lock;
        ASSERT_TRUE(sched::wake_no_resched(b)) << "fixture: the thread was parked";
        ASSERT_EQ(b->state, ThreadState::HANDED) << "fixture: the thread is bound for the peer";
        sched::set_prio(b, PRIO_BOOST);
    }
    g_counted = *sched::default_policy();
    g_counted.on_remove = counted_on_remove;
    g_reseats = 0;
    kernel().policy = &g_counted;

    dispatch_as(CORE_PEER);

    EXPECT_TRUE(linked_on(b, CORE_PEER)) << "the handoff was not linked";
    EXPECT_EQ(b->rq_prio, PRIO_BOOST) << "the thread runs keyed on a priority it no longer has";
    EXPECT_EQ(g_reseats, 0u)
        << "the link keyed the thread on the priority it had when it was handed, and only the "
           "request behind it re-seated it";
    EXPECT_EQ(b->reseat_owed, 0u) << "the request behind the handoff was never retired";
}

TEST_F(SchedRing, a_reseat_reaching_a_core_that_moved_the_thread_is_dropped_and_the_link_reads_the_field)
{
    OnPeer o = ready_on_peer(PRIO_UNDER);
    o.t->affinity = KICKOS_CORE_SET_ALL;
    set_prio_as(CORE_ME, o.t, PRIO_UNDER + 1u);
    ASSERT_EQ(o.t->reseat_owed, 1u) << "fixture: the request is in flight";
    // The peer moves the thread here itself, a re-mask off the peer applied where the thread is
    // linked, before the peer's dispatch reads the request.
    uint32_t const was = g_core;
    g_core = CORE_PEER;
    {
        IrqLock lock;
        sched::set_affinity(o.t, 1u << CORE_ME);
    }
    g_core = was;
    ASSERT_EQ(o.t->state, ThreadState::HANDED) << "fixture: the peer moved the thread here";
    ASSERT_EQ(o.t->queue_core, CORE_ME) << "fixture: the peer moved the thread here";

    dispatch_as(CORE_PEER);

    EXPECT_EQ(o.t->reseat_owed, 0u) << "the request for a thread that moved was never retired";
    EXPECT_EQ(o.t->state, ThreadState::HANDED) << "the request touched a thread that moved";
    EXPECT_FALSE(linked_on(o.t, CORE_PEER)) << "the request re-linked a thread that moved";

    dispatch_as(CORE_ME);

    EXPECT_TRUE(linked_on(o.t, CORE_ME)) << "the core it was pushed to never linked it";
    EXPECT_EQ(o.t->rq_prio, PRIO_UNDER + 1u) << "the link did not read the field the request wrote";
}

TEST_F(SchedRing, a_reseat_of_a_parked_or_exited_thread_is_dropped)
{
    OnPeer o = ready_on_peer(PRIO_UNDER);
    set_prio_as(CORE_ME, o.t, PRIO_UNDER + 1u);
    uint32_t const was = g_core;
    g_core = CORE_PEER;
    {
        IrqLock lock;
        park_join(o.t, o.runner);
    }
    g_core = was;

    dispatch_as(CORE_PEER);

    EXPECT_EQ(o.t->reseat_owed, 0u) << "the request for a parked thread was never retired";
    EXPECT_EQ(o.t->state, ThreadState::BLOCKED) << "the request touched a parked thread";
    EXPECT_FALSE(linked_anywhere(o.t)) << "the request linked a parked thread";

    // The same request against a thread that exited meanwhile.
    reset();
    OnPeer q = ready_on_peer(PRIO_UNDER);
    set_prio_as(CORE_ME, q.t, PRIO_UNDER + 1u);
    kernel().policy->on_remove(q.t);
    q.t->state = ThreadState::EXITED;

    dispatch_as(CORE_PEER);

    EXPECT_EQ(q.t->reseat_owed, 0u) << "the request for an exited thread was never retired";
    EXPECT_EQ(q.t->state, ThreadState::EXITED) << "the request touched an exited thread";
    EXPECT_FALSE(linked_anywhere(q.t)) << "the request linked an exited thread";
}

TEST_F(SchedRing, a_second_request_before_the_first_is_retired_publishes_nothing)
{
    OnPeer o = ready_on_peer(PRIO_UNDER);

    set_prio_as(CORE_ME, o.t, PRIO_UNDER + 1u);
    sched::set_affinity(o.t, KICKOS_CORE_SET_ALL);
    set_prio_as(CORE_ME, o.t, PRIO_BOOST);

    EXPECT_EQ(in_flight(CORE_ME, CORE_PEER), 1u)
        << "a second request was staged while the first was in flight, which is what lets a "
           "ring fill: the depth is sized for one request per slot";

    dispatch_as(CORE_PEER);

    EXPECT_EQ(o.t->rq_prio, PRIO_BOOST) << "the one request did not read the last write";
}

TEST_F(SchedRing, a_local_request_on_a_thread_handed_to_this_core_writes_only_the_field)
{
    Thread* const runner = runner_and_peer(true);
    Thread* const b = parked(2, PRIO_UNDER, runner, 1u << CORE_ME);
    uint32_t const was = g_core;
    g_core = CORE_PEER;
    {
        IrqLock lock;
        ASSERT_TRUE(sched::wake_no_resched(b)) << "fixture: the thread was parked";
    }
    g_core = was;
    ASSERT_EQ(b->state, ThreadState::HANDED) << "fixture: the peer handed the thread here";
    ASSERT_EQ(b->queue_core, CORE_ME) << "fixture: the peer handed the thread here";

    set_prio_as(CORE_ME, b, PRIO_BOOST);

    EXPECT_EQ(b->prio, PRIO_BOOST) << "the request did not write the field";
    EXPECT_EQ(b->reseat_owed, 0u) << "a request was staged for a thread this core will link";
    EXPECT_EQ(in_flight(CORE_ME, CORE_PEER), 0u) << "a request went to a core the thread left";

    dispatch_as(CORE_ME);

    EXPECT_EQ(b->rq_prio, PRIO_BOOST) << "the link did not read the field";
}

TEST_F(SchedRing, a_reseat_in_flight_across_a_slot_reuse_does_not_admit_a_second)
{
    OnPeer o = ready_on_peer(PRIO_UNDER);
    set_prio_as(CORE_ME, o.t, PRIO_UNDER + 1u);
    ASSERT_EQ(o.t->reseat_owed, 1u) << "fixture: the request is in flight";
    // The occupant dies with its request in flight and the slot is taken by the next spawn.
    int const slot = kernel().threads.index_of(o.t);
    kernel().policy->on_remove(o.t);
    o.t->state = ThreadState::EXITED;
    Thread* const heir = seat_pool(slot, PRIO_UNDER);
    ASSERT_EQ(heir->reseat_owed, 1u)
        << "the slot's zeroing for its next occupant dropped the byte an entry in flight still "
           "names, so a second request for that slot can be staged beside it";
    seat_running_on(heir, CORE_PEER);

    set_prio_as(CORE_ME, heir, PRIO_BOOST);

    EXPECT_EQ(in_flight(CORE_ME, CORE_PEER), 1u) << "a second request for one slot was staged";

    dispatch_as(CORE_PEER);

    EXPECT_EQ(heir->reseat_owed, 0u) << "the request the new occupant inherited was never retired";
    EXPECT_EQ(heir->rq_prio, PRIO_BOOST) << "the inherited request did not re-read the new occupant";
}

TEST_F(SchedRing, a_boost_requested_after_a_move_follows_the_thread_to_its_new_core)
{
    OnPeer o = moved_here_behind_a_request(PRIO_UNDER + 1u);
    ASSERT_TRUE(linked_on(o.t, CORE_ME)) << "fixture: this core linked the moved thread";
    ASSERT_EQ(o.t->reseat_owed, 1u) << "fixture: request #1 is still unretired";
    ASSERT_EQ(in_flight(CORE_ME, CORE_PEER), 1u) << "fixture: request #1 is still on the ring";

    set_prio_as(CORE_PEER, o.t, PRIO_BOOST);
    dispatch_as(CORE_PEER);

    EXPECT_EQ(o.t->reseat_owed, 1u) << "the peer retired the slot with a request still owed";
    EXPECT_EQ(in_flight(CORE_PEER, CORE_ME), 1u)
        << "the peer dropped the entry for a thread linked here, and the boost it suppressed "
           "reaches nobody";

    dispatch_as(CORE_ME);

    EXPECT_EQ(o.t->rq_prio, PRIO_BOOST) << "the boost never reached the list the thread sits on";
    EXPECT_EQ(o.t->reseat_owed, 0u) << "the forwarded request left its slot owed";
    EXPECT_EQ(kernel().current[CORE_ME], o.t) << "the boosted thread does not run over the runner";
}

TEST_F(SchedRing, a_narrowing_requested_after_a_move_follows_the_thread_to_its_new_core)
{
    OnPeer o = moved_here_behind_a_request(PRIO_UNDER + 1u);
    ASSERT_TRUE(linked_on(o.t, CORE_ME)) << "fixture: this core linked the moved thread";
    ASSERT_EQ(o.t->reseat_owed, 1u) << "fixture: request #1 is still unretired";

    set_affinity_as(CORE_PEER, o.t, 1u << CORE_PEER);
    dispatch_as(CORE_PEER);
    dispatch_as(CORE_ME);
    dispatch_as(CORE_PEER);

    EXPECT_EQ(o.t->queue_core, CORE_PEER)
        << "the thread stays linked on a core its mask no longer names";
    EXPECT_TRUE(linked_on(o.t, CORE_PEER)) << "the core the mask names never linked it";
    EXPECT_EQ(o.t->reseat_owed, 0u) << "the forwarded request left its slot owed";
}

TEST_F(SchedRing, a_slay_requested_after_a_move_follows_the_thread_to_its_new_core)
{
    OnPeer o = moved_here_behind_a_request(PRIO_BOOST);
    ASSERT_EQ(kernel().current[CORE_ME], o.t) << "fixture: the moved thread runs here";
    ASSERT_EQ(o.t->reseat_owed, 1u) << "fixture: request #1 is still unretired";
    g_redirect_target = nullptr;

    slay_as(CORE_PEER, o.t);
    dispatch_as(CORE_PEER);
    dispatch_as(CORE_ME);

    EXPECT_NE(kernel().current[CORE_ME], o.t)
        << "the slain thread keeps its core: the peer dropped the request for a thread running "
           "here";

    dispatch_as(CORE_ME);

    EXPECT_EQ(g_redirect_target, o.t) << "the pass this core owed itself never claimed the victim";
}
