// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Where a READY thread is placed across cores, at two kernel cores, over the real pick, the
// real switch_book and the real FIFO policy.
//
// The invariant: a READY thread never waits behind equal or higher priority while a started core
// in its mask has a level strictly below it. A same-core handoff is had by pinning. Two halves
// enforce it and both are read here:
//
//   the decline  a pass that leaves a READY thread behind an equal or higher one moves it, from
//                the holder's own structure, to the lowest core strictly below it and asks
//                that core.
//   the drop     a core whose level falls asks the holder of a thread it would now take to
//                push it, and the holder does on its next pass. No core takes a thread off
//                another's structure.
//
// The ask is a counter before it is a doorbell (kernel/sync/klock.cc), and a counter is
// readable on the host, where the doorbell is not.

#include <kickos/arch/arch.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/klock.h>
#include <kickos/sched.h>
#include <kickos/thread.h>

#include "kseam_test.h"

using namespace kickos;
using namespace kickos::testfix;

namespace
{
    constexpr uint32_t CORE_ME = 0;   // the core the fixture speaks as
    constexpr uint32_t CORE_PEER = 1; // the other core

    // Not ROOT_INDEX: ThreadPool::alloc retires root's slot.
    constexpr int SLOT_RUNNER = 1;
    constexpr int SLOT_HOG = 2;
    constexpr int SLOT_T = 3;
    constexpr int SLOT_U = 4;
    // Fixture storage rather than a pool slot: nothing scans for an idle thread.
    constexpr int SLOT_PEER_IDLE = 0;

    constexpr uint8_t PRIO_RUNNER = 6;
    constexpr uint8_t PRIO_UNDER = 5;
    constexpr uint8_t PRIO_LOW = 4;
    constexpr uint8_t PRIO_PEER_LOW = 3;
    constexpr uint8_t PRIO_HOG = 7;
    static_assert(PRIO_PEER_LOW < PRIO_LOW and PRIO_LOW < PRIO_UNDER and PRIO_UNDER < PRIO_RUNNER
                      and PRIO_RUNNER < PRIO_HOG,
                  "every arm below reads one ordering of these five; a reorder makes an arm "
                  "assert about whichever thread its scan reached first");

    // The ask cell is keyed by target and read from the target's seat.
    int owed_at(uint32_t core)
    {
        uint32_t const was = g_core;
        g_core = core;
        int const owed = kickos_kernel_core_resched_owed();
        g_core = was;
        return owed;
    }

    // klock.cc's sequence rows outlive reset(), so an arm starts from what it drains here.
    void drain(uint32_t core)
    {
        uint32_t const was = g_core;
        g_core = core;
        (void)kickos_kernel_core_resched_take();
        g_core = was;
    }

    void settle()
    {
        drain(CORE_ME);
        drain(CORE_PEER);
    }

    // pick_next's fallback is idle[core] unconditionally, and the fixture seats one for the
    // boot core alone. Added only: the peer has not started until something seats its current.
    Thread* add_peer_idle()
    {
        Thread* const t = &g_fx.t[SLOT_PEER_IDLE];
        t->base_prio = KICKOS_PRIO_IDLE;
        t->prio = KICKOS_PRIO_IDLE;
        t->id = static_cast<uint16_t>(SLOT_PEER_IDLE + 1);
        sched::add_idle(t, CORE_PEER);
        return t;
    }

    // As one core, for one scheduler pass. The seat travels with the identity: every keyed read
    // in the compiled sources resolves through g_core.
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

    // The fixture refuses a park with no waker armed; a thread parked here stays parked.
    void leave_parked(Thread*)
    {
    }

    struct Duo
    {
        Thread* runner;    // RUNNING on CORE_ME at PRIO_RUNNER
        Thread* peer_idle; // RUNNING on CORE_PEER
    };

    // The peer started and idle, so its level is the lowest there is.
    Duo duo()
    {
        Duo d{};
        d.runner = seat_pool(SLOT_RUNNER, PRIO_RUNNER);
        d.peer_idle = add_peer_idle();
        {
            IrqLock lock;
            sched::reschedule();
        }
        seat_running_on(d.peer_idle, CORE_PEER);
        settle();
        return d;
    }

    // A thread parked on nothing it can be woken from except by the arm.
    Thread* parked(int slot, uint8_t prio, Thread* on)
    {
        Thread* const t = seat_pool(slot, prio);
        park_join(t, on);
        settle();
        return t;
    }

    struct Held
    {
        Thread* runner; // RUNNING on CORE_ME at PRIO_RUNNER
        Thread* hog;    // RUNNING on CORE_PEER at PRIO_HOG, above everything CORE_ME holds
        Thread* t;      // READY on CORE_ME: declined by both cores when it was placed
    };

    // Both cores at or above `t`, so it stays with the core that readied it. The hog is seated
    // after this core's pass and before the peer starts, or one of them would run it here.
    Held held(uint8_t t_prio)
    {
        Held h{};
        h.runner = seat_pool(SLOT_RUNNER, PRIO_RUNNER);
        (void)add_peer_idle();
        {
            IrqLock lock;
            sched::reschedule();
        }
        h.hog = seat_pool(SLOT_HOG, PRIO_HOG);
        seat_running_on(h.hog, CORE_PEER);
        h.t = seat_pool(SLOT_T, t_prio);
        settle();
        return h;
    }

    // The peer's hog parks and the peer's own pass seats its idle, which is the drop.
    void hog_parks(Held const& h)
    {
        uint32_t const was = g_core;
        g_core = CORE_PEER;
        {
            IrqLock lock;
            park_join(h.hog, h.runner);
        }
        wake_next_park(leave_parked);
        g_core = was;
        pass_as(CORE_PEER);
    }
}

class Placement : public kickos::testfix::KSeam
{
};

// A wide mask is the caller asking for spread, so an equal-priority wake does not wait for its
// waker to park while a core in that mask idles.
TEST_F(Placement, a_declined_equal_priority_wake_moves_to_an_idle_core)
{
    Duo d = duo();
    Thread* const b = parked(SLOT_T, PRIO_RUNNER, d.runner);

    wake_as(CORE_ME, b);

    ASSERT_EQ(kernel().current[CORE_ME], d.runner)
        << "fixture: the running thread is ahead of an equal one, so this core declines the wake";
    EXPECT_EQ(b->queue_core, CORE_PEER)
        << "an equal-priority wake with a wide mask waits for its waker to park while a core it "
           "may run on idles";
    EXPECT_NE(owed_at(CORE_PEER), 0) << "the thread was moved to a core nobody told to look";
}

// The same-core handoff: two threads pinned to one core. The wake stays and asks nobody,
// whatever idles beside it.
TEST_F(Placement, an_equal_priority_wake_between_threads_pinned_to_one_core_stays_and_asks_nobody)
{
    Duo d = duo();
    d.runner->affinity = 1u << CORE_ME;
    Thread* const b = parked(SLOT_T, PRIO_RUNNER, d.runner);
    b->affinity = 1u << CORE_ME;

    wake_as(CORE_ME, b);

    ASSERT_EQ(kernel().current[CORE_ME], d.runner)
        << "fixture: the running thread is ahead of an equal one, so this core declines the wake";
    EXPECT_EQ(b->queue_core, CORE_ME) << "a thread pinned to this core was placed elsewhere";
    EXPECT_EQ(owed_at(CORE_PEER), 0)
        << "a peer the pinned thread may not run on was asked to look for it";
}

TEST_F(Placement, a_wake_declined_by_a_strictly_higher_current_is_pushed_to_an_idle_core)
{
    Duo d = duo();
    Thread* const b = parked(SLOT_T, PRIO_UNDER, d.runner);

    wake_as(CORE_ME, b);

    ASSERT_EQ(kernel().current[CORE_ME], d.runner)
        << "fixture: the running thread outranks the woken one";
    EXPECT_EQ(b->queue_core, CORE_PEER)
        << "the woken thread waits behind a higher priority while an idle core in its mask "
           "runs nothing, and under FIFO nothing later is owed that would move it";
    EXPECT_NE(owed_at(CORE_PEER), 0)
        << "the thread was moved to a core nobody told to look, so it waits there instead";
}

// The selftest's hang, on the host: a wake whose waker is exiting has no pass to decline it in,
// and the exit's own pass then seats something else over it.
TEST_F(Placement, a_wake_from_an_exiting_thread_is_placed_by_the_pass_it_cannot_take)
{
    constexpr uint8_t PRIO_EXITING = 20;
    constexpr uint8_t PRIO_SPINNER = 14;
    constexpr uint8_t PRIO_JOINER = 10;
    Thread* const exiting = seat_pool(SLOT_RUNNER, PRIO_EXITING);
    Thread* const spinner = seat_pool(SLOT_HOG, PRIO_SPINNER);
    Thread* const joiner = seat_pool(SLOT_T, PRIO_JOINER);
    Thread* const peer_idle = add_peer_idle();
    {
        IrqLock lock;
        sched::reschedule();
    }
    ASSERT_EQ(kernel().current[CORE_ME], exiting) << "fixture: the exiting thread runs here";
    park_join(joiner, exiting);
    spinner->affinity = 1u << CORE_ME;
    seat_running_on(peer_idle, CORE_PEER);
    settle();

    run_exit(0);

    ASSERT_EQ(kernel().current[CORE_ME], spinner)
        << "fixture: the exit's own pass seats the thread above the joiner";
    ASSERT_EQ(joiner->state, ThreadState::READY) << "fixture: the exit woke its joiner";
    EXPECT_EQ(joiner->queue_core, CORE_PEER)
        << "the joiner waits behind the spinner on the exiting thread's core while the peer "
           "idles, and nothing later is owed that would move it";
    EXPECT_NE(owed_at(CORE_PEER), 0) << "the joiner was moved to a core nobody told to look";
}

// Threads readied while this core runs below them all expect its next pass, and that pass takes
// one. The rest are declined by it without being named to it, as the woken thread is.
TEST_F(Placement, threads_readied_ahead_of_one_pass_are_placed_by_the_pass_that_takes_another)
{
    constexpr uint8_t PRIO_SPAWNER = 2;
    Thread* const spawner = seat_pool(SLOT_RUNNER, PRIO_SPAWNER);
    Thread* const peer_idle = add_peer_idle();
    {
        IrqLock lock;
        sched::reschedule();
    }
    seat_running_on(peer_idle, CORE_PEER);
    settle();
    Thread* const low = seat_pool(SLOT_T, PRIO_LOW);
    Thread* const under = seat_pool(SLOT_U, PRIO_UNDER);
    Thread* const top = seat_pool(SLOT_HOG, PRIO_RUNNER);
    ASSERT_EQ(under->queue_core, CORE_ME) << "fixture: this core was below each as it arrived";
    ASSERT_EQ(owed_at(CORE_PEER), 0) << "fixture: arriving asked nobody";
    {
        IrqLock lock;
        park_join(spawner, top);
    }
    wake_next_park(leave_parked);
    settle();

    pass_as(CORE_ME);

    ASSERT_EQ(kernel().current[CORE_ME], top) << "fixture: the pass takes the highest";
    EXPECT_EQ(under->queue_core, CORE_PEER)
        << "a thread that expected this pass waits behind the one it took while the peer "
           "idles, and no later event is owed that would move it";
    EXPECT_NE(owed_at(CORE_PEER), 0) << "the thread was moved to a core nobody told to look";
    EXPECT_EQ(low->queue_core, CORE_ME)
        << "the second declined thread followed the first onto a core that now runs above it";
}

// A core's level is what its next pass seats, READY threads included, so the first thread a
// pass places on an idle core raises that core for the second.
TEST_F(Placement, two_threads_declined_in_one_pass_do_not_pile_onto_one_idle_core)
{
    Duo d = duo();
    Thread* const under = parked(SLOT_T, PRIO_UNDER, d.runner);
    Thread* const low = parked(SLOT_U, PRIO_LOW, d.runner);

    {
        IrqLock lock;
        ASSERT_TRUE(sched::wake_no_resched(under));
        ASSERT_TRUE(sched::wake_no_resched(low));
        sched::place_ready(under);
        sched::place_ready(low);
    }

    ASSERT_EQ(under->queue_core, CORE_PEER) << "fixture: the first goes to the idle core";
    EXPECT_EQ(low->queue_core, CORE_ME)
        << "the second thread was moved to a core that will run the first ahead of it, which "
           "the placement read as idle from its running thread alone";
}

// The drop: a thread both cores declined stays with the core that readied it, and the core
// that later falls below it is not its holder, so nothing on the holder's side notices.
TEST_F(Placement, a_core_that_falls_below_a_peers_ready_thread_asks_that_peer_to_push_it)
{
    Held h = held(PRIO_UNDER);
    ASSERT_EQ(h.t->queue_core, CORE_ME) << "fixture: both cores were at or above the thread";
    ASSERT_EQ(owed_at(CORE_PEER), 0) << "fixture: placing it asked nobody";

    hog_parks(h);

    ASSERT_EQ(kernel().current[CORE_PEER], kernel().idle[CORE_PEER])
        << "fixture: the peer fell to its idle";
    EXPECT_NE(owed_at(CORE_ME), 0)
        << "the peer fell below a thread waiting behind a higher priority here and asked this "
           "core nothing, so the thread waits for as long as FIFO keeps the runner on the CPU";
    EXPECT_EQ(kernel().push_asked[CORE_ME], 1u << CORE_PEER)
        << "the ask carried no request naming the core that fell";

    settle();
    pass_as(CORE_ME);

    ASSERT_EQ(kernel().current[CORE_ME], h.runner) << "fixture: this core keeps its runner";
    EXPECT_EQ(h.t->queue_core, CORE_PEER)
        << "the holder took the request and pushed nothing, so the drop was asked for and lost";
    EXPECT_NE(owed_at(CORE_PEER), 0) << "the thread was pushed to a core nobody told to look";

    pass_as(CORE_PEER);

    EXPECT_EQ(kernel().current[CORE_PEER], h.t) << "the pushed thread was not the peer's pick";
}

TEST_F(Placement, a_drop_uncovering_only_threads_pinned_elsewhere_asks_nobody)
{
    Held h = held(PRIO_UNDER);
    h.t->affinity = 1u << CORE_ME;

    hog_parks(h);

    ASSERT_EQ(kernel().current[CORE_PEER], kernel().idle[CORE_PEER])
        << "fixture: the peer fell to its idle";
    EXPECT_EQ(owed_at(CORE_ME), 0)
        << "a core was asked to push a thread whose mask does not name the core that fell";
    EXPECT_EQ(kernel().push_asked[CORE_ME], 0u) << "a request stands for a thread nobody may take";
}

// Strictly above: a core falling to a thread's priority would run it only behind what it
// already runs, which is the equal wait the thread is allowed on its own core.
TEST_F(Placement, a_drop_to_a_threads_own_priority_is_not_a_drop_for_that_thread)
{
    Held h = held(PRIO_UNDER);
    {
        IrqLock lock;
        sched::set_prio(h.hog, PRIO_UNDER);
    }
    settle();

    pass_as(CORE_PEER);

    ASSERT_EQ(kernel().current[CORE_PEER], h.hog) << "fixture: the peer keeps its lowered hog";
    EXPECT_EQ(owed_at(CORE_ME), 0)
        << "a core that fell only to the thread's own priority asked for it, so the thread "
           "would trade an equal wait here for an equal wait there at the cost of a push";
}

TEST_F(Placement, a_thread_waiting_behind_an_equal_on_its_own_core_is_pushed_to_a_dropping_core)
{
    Held h = held(PRIO_RUNNER);

    hog_parks(h);

    ASSERT_EQ(kernel().current[CORE_PEER], kernel().idle[CORE_PEER])
        << "fixture: the peer fell to its idle";
    EXPECT_NE(owed_at(CORE_ME), 0)
        << "the peer fell below a thread waiting behind an equal here and asked this core "
           "nothing, so the thread waits for the runner to park while the peer idles";

    settle();
    pass_as(CORE_ME);

    EXPECT_EQ(h.t->queue_core, CORE_PEER) << "the holder did not push what the fallen core asked";
}

// A lowering of a running thread moves no list a peer's placement would see until that core's
// own pass seats it, so the lowering asks that pass for it and leaves the seat's record high.
TEST_F(Placement, lowering_a_peers_running_thread_asks_that_peer_for_the_pass_that_sees_the_drop)
{
    Held h = held(PRIO_UNDER);

    {
        IrqLock lock;
        sched::set_prio(h.hog, PRIO_PEER_LOW);
    }

    EXPECT_NE(owed_at(CORE_PEER), 0)
        << "the peer's running thread fell below a thread this core holds and the peer was "
           "not asked for the pass that would see it";

    settle();
    pass_as(CORE_PEER);

    ASSERT_EQ(kernel().current[CORE_PEER], h.hog) << "fixture: the peer keeps its hog";
    EXPECT_NE(owed_at(CORE_ME), 0)
        << "the peer's pass seated the lowered thread and saw no drop, its record of what it "
           "seated having followed the lowering down";
}

// A core that has not started has no scheduler to take a doorbell with, so nothing is placed
// there; and its start is itself a fall from nothing, so what waits for it is asked for then.
TEST_F(Placement, a_core_is_placed_on_only_once_started_and_asks_for_what_waits_when_it_starts)
{
    Thread* const runner = seat_pool(SLOT_RUNNER, PRIO_RUNNER);
    (void)add_peer_idle();
    {
        IrqLock lock;
        sched::reschedule();
    }
    ASSERT_EQ(kernel().current[CORE_ME], runner) << "fixture: the runner runs here";
    ASSERT_EQ(kernel().current[CORE_PEER], nullptr) << "fixture: the peer has not started";

    Thread* const t = seat_pool(SLOT_T, PRIO_UNDER);

    EXPECT_EQ(t->queue_core, CORE_ME)
        << "a thread was placed on a core that cannot yet take the doorbell asking it to look";
    EXPECT_EQ(owed_at(CORE_PEER), 0) << "a core that has not started was asked for a pass";

    settle();
    g_core = CORE_PEER;
    sched::start();
    g_core = CORE_ME;

    ASSERT_EQ(kernel().current[CORE_PEER], kernel().idle[CORE_PEER])
        << "fixture: the peer started on its idle";
    EXPECT_NE(owed_at(CORE_ME), 0)
        << "the peer started below a thread waiting behind a higher priority here and asked "
           "for nothing, so the thread strands on a core that never looks again";

    pass_as(CORE_ME);

    EXPECT_EQ(t->queue_core, CORE_PEER) << "the holder did not push what the started core asked";
}

// The request is a hint and the holder's pass re-reads it: a core whose level rose again before
// the holder looked is no longer below the thread and gets nothing.
TEST_F(Placement, a_push_is_not_made_once_the_core_that_asked_no_longer_sits_below)
{
    Held h = held(PRIO_UNDER);
    h.hog->affinity = 1u << CORE_PEER;
    hog_parks(h);
    ASSERT_EQ(kernel().push_asked[CORE_ME], 1u << CORE_PEER)
        << "fixture: the peer's fall stands as a request";

    wake_as(CORE_PEER, h.hog);
    ASSERT_EQ(kernel().current[CORE_PEER], h.hog) << "fixture: the peer rose again";
    settle();

    pass_as(CORE_ME);

    EXPECT_EQ(h.t->queue_core, CORE_ME)
        << "the holder pushed on a stale request, onto a core now running above the thread";
    EXPECT_EQ(owed_at(CORE_PEER), 0) << "a core above the thread was asked to take it";
    EXPECT_EQ(kernel().push_asked[CORE_ME], 0u) << "the holder's pass left the request standing";
}

namespace
{
    // The FIFO decline walk, watched: once it has answered `g_after`, the thread `g_passed`
    // before it is widened, which only a walk revisiting what it already passed can see.
    SchedPolicy g_watched{};
    Thread* g_passed = nullptr;
    Thread const* g_after = nullptr;
    bool g_after_answered = false;
    uint32_t g_passed_answers = 0;

    Thread* watched_declined(uint32_t core, int above, SchedWalk* walk)
    {
        if (g_after_answered)
        {
            g_passed->affinity = KICKOS_CORE_SET_ALL;
        }
        Thread* const t = sched::default_policy()->declined(core, above, walk);
        if (t == g_after)
        {
            g_after_answered = true;
        }
        if (t == g_passed)
        {
            g_passed_answers++;
        }
        return t;
    }
}

// Each READY thread in the band is visited once per pass. A walk that restarts from the top of
// the band after each thread it moves re-reads every thread ahead of it, which is quadratic.
TEST_F(Placement, a_decline_pass_does_not_revisit_a_thread_it_already_passed)
{
    constexpr uint8_t PRIO_SPAWNER = 2;
    Thread* const spawner = seat_pool(SLOT_RUNNER, PRIO_SPAWNER);
    Thread* const peer_idle = add_peer_idle();
    {
        IrqLock lock;
        sched::reschedule();
    }
    Thread* const pinned = seat_pool(SLOT_T, PRIO_UNDER);
    pinned->affinity = 1u << CORE_ME;
    Thread* const wide = seat_pool(SLOT_U, PRIO_UNDER);
    Thread* const top = seat_pool(SLOT_HOG, PRIO_RUNNER);
    ASSERT_EQ(wide->queue_core, CORE_ME) << "fixture: the peer had not started as each arrived";
    seat_running_on(peer_idle, CORE_PEER);
    {
        IrqLock lock;
        park_join(spawner, top);
    }
    wake_next_park(leave_parked);
    settle();
    g_watched = *sched::default_policy();
    g_watched.declined = watched_declined;
    g_passed = pinned;
    g_after = wide;
    g_after_answered = false;
    g_passed_answers = 0;
    kernel().policy = &g_watched;

    pass_as(CORE_ME);

    ASSERT_EQ(kernel().current[CORE_ME], top) << "fixture: the pass takes the highest";
    ASSERT_TRUE(g_after_answered) << "fixture: the walk never reached the wide thread";
    EXPECT_EQ(wide->queue_core, CORE_PEER) << "the wide thread waits behind `top` while the peer idles";
    EXPECT_EQ(g_passed_answers, 0u)
        << "the walk returned to a thread it had passed before the one it moved";
}
