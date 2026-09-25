// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The cross-core ask a re-placement owes, at two kernel cores, over the real
// sched::set_affinity and the real switch_book.
//
// A thread the new mask excludes from the core it executes on is never yanked: it stays
// RUNNING until a switch stores READY, and every peer's pick_next refuses a RUNNING thread.
// So the ask that tells its new cores to look cannot be sent when the mask changes; it has
// to ride on the far side of that store. The ask is a counter before it is a doorbell
// (kernel/sync/klock.cc), and a counter is readable on the host, where the doorbell is not.
//
// Four lines:
//   the place    switch_book's `place(displaced, me)`, the ask that follows the store making
//                the thread takeable. One line for every switch shape, so most arms below
//                reach it: an ordinary reschedule, a re-mask off this core and a wake this
//                core takes all differ in what brought the switch and in nothing the store
//                made available. The thread is placed at its priority, never the woken
//                thread's, and an outgoing thread that parked or exited never reaches the store.
//   the guard    switch_book's `(prev->affinity & ~(1u << me)) != 0`, which skips placing a
//                thread no peer may run. It removes no ask the placement would have sent, so an
//                arm proving it must read the placement's own mask test.
//   the pass     set_affinity's `klock_resched_ask(1u << core)`, a different ask entirely:
//                the thread runs on a peer, and only that peer's own pass can move it.
//   the wake     pick_and_seat's `if (woken != nullptr and next != woken)`, the same cell read
//                from the other side: a wake this core takes owes no peer anything about the
//                woken thread, whose seat is published before the lock that hides it is
//                released.
//
// A thread is asked for only where it waits behind equal or higher priority and a peer runs
// strictly below it. An arm expecting an ask stages the first, and the peer running PRIO_BELOW
// stages the second.

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
    constexpr uint32_t CORE_PEER = 1; // the core an ask is read against

    constexpr uint8_t PRIO_RUNNER = 6;
    constexpr uint8_t PRIO_UNDER_RUNNER = 5;
    constexpr uint8_t PRIO_BELOW = 3;
    constexpr uint8_t PRIO_BETWEEN = 7;
    constexpr uint8_t PRIO_ABOVE = 8;
    constexpr uint8_t PRIO_TOP = 9;
    static_assert(PRIO_BELOW < PRIO_UNDER_RUNNER and PRIO_UNDER_RUNNER < PRIO_RUNNER,
                  "a thread under the runner must still outrank the peer, or no core is below "
                  "it and the arm that stages it asserts nothing");

    // The ask cell is keyed by target and read from the target's seat, so an arm asking
    // about a peer has to speak as that peer for the length of the read.
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

    struct Placed
    {
        Thread* runner; // RUNNING on CORE_ME
        Thread* alt;    // READY at the same priority, so a switch off `runner` has somewhere to go
        Thread* below;  // RUNNING on CORE_PEER, under `runner`
    };

    // A peer running below is what makes an ask expressible at all: placement never moves a
    // thread to a core running at or above it, and an arm arranged without one asserts nothing.
    Placed place()
    {
        Placed p{};
        p.runner = seat_pool(0, PRIO_RUNNER);
        p.alt = seat_pool(1, PRIO_RUNNER);
        p.below = seat_pool(2, PRIO_BELOW);
        {
            IrqLock lock;
            sched::reschedule();
        }

        detach_ready(p.below);
        p.below->state = ThreadState::RUNNING;
        kickos::testfix::seat_running_on(p.below, CORE_PEER);

        drain(CORE_ME);
        drain(CORE_PEER);
        return p;
    }

    // The fixture refuses a park with no waker armed, and an arm about the outgoing thread's
    // state needs it to still be parked when the assertion reads it.
    void leave_parked(Thread*)
    {
    }
}

class MigrateAsk : public kickos::testfix::KSeam
{
};

TEST_F(MigrateAsk, a_thread_re_masked_off_the_core_it_runs_on_asks_the_core_it_may_now_take)
{
    Placed p = place();
    ASSERT_EQ(kernel().current[CORE_ME], p.runner) << "fixture: the thread runs on this core";
    ASSERT_EQ(owed_at(CORE_PEER), 0) << "fixture: nothing stands against the peer core";

    sched::set_affinity(p.runner, 1u << CORE_PEER);

    ASSERT_EQ(kernel().current[CORE_ME], p.alt)
        << "fixture: the re-mask cost this core the switch the ask rides behind";
    ASSERT_EQ(p.runner->state, ThreadState::HANDED)
        << "fixture: that switch handed the thread to the peer, whose drain links it";
    EXPECT_NE(owed_at(CORE_PEER), 0)
        << "the thread is READY, eligible only on the peer core, and that core was never told "
           "to look: it runs a thread below and will keep running it until its next natural "
           "switch, so the re-placement is worth whatever the peer's timer happens to owe it";
}

// The switch nothing else announces. A thread that widens its affinity and is then displaced by
// a priority raise is READY, eligible on a peer and above what that peer runs, with no wake and
// no re-mask left to carry the ask: the ordinary reschedule is the only thing that can send it.
// The two halves are one variable apart, the outgoing thread's mask, so the second is what says
// the first reads that mask rather than firing on any switch at all.
namespace
{
    // A raise with no wake behind it, then the ordinary pass that answers it.
    void displace_runner_by_a_raise(Placed const& p)
    {
        IrqLock lock;
        sched::set_prio(p.alt, PRIO_ABOVE);
        sched::reschedule();
    }
}

TEST_F(MigrateAsk, an_ordinary_switch_asks_for_the_thread_it_displaces)
{
    Placed p = place();
    // Narrowed by hand, so the call below is a widening that leaves the running core in.
    p.runner->affinity = 1u << CORE_ME;

    sched::set_affinity(p.runner, KICKOS_CORE_SET_ALL);
    ASSERT_EQ(kernel().current[CORE_ME], p.runner)
        << "fixture: a mask that still admits this core moves nothing";
    ASSERT_EQ(owed_at(CORE_PEER), 0)
        << "fixture: the widening itself owes nothing, the thread being RUNNING throughout it";

    displace_runner_by_a_raise(p);
    ASSERT_EQ(kernel().current[CORE_ME], p.alt) << "fixture: the pass took the switch";
    ASSERT_EQ(p.runner->state, ThreadState::HANDED)
        << "fixture: that switch handed the thread to the peer, whose drain links it";

    EXPECT_NE(owed_at(CORE_PEER), 0)
        << "an ordinary switch put a thread the peer core may run into the ready set above "
           "what that core runs, and told nobody. The widening could not have asked and no "
           "wake follows, so this switch is the last event that could: the peer keeps the "
           "lower-priority thread on the CPU for as long as FIFO leaves it there, which under "
           "no further event is forever";

    // The same switch with the outgoing mask the only thing changed.
    reset();
    Placed q = place();
    q.runner->affinity = 1u << CORE_ME;

    displace_runner_by_a_raise(q);
    ASSERT_EQ(kernel().current[CORE_ME], q.alt) << "fixture: the pass took the switch";
    ASSERT_EQ(q.runner->state, ThreadState::READY)
        << "fixture: and stored the same state over the thread it displaced";

    EXPECT_EQ(owed_at(CORE_PEER), 0)
        << "a peer was asked to look for a thread pinned to the core it just left. It would "
           "take the raise, contend the kernel lock, find the ready set it already declined "
           "and re-pick what it was running; without this half the assertion above would pass "
           "for a switch that asked on every pass regardless of placement";
}

// A thread no core can pick gets no ask, and the same thread READY does: the two halves are
// what separate "the ask is narrowed by run state" from "nothing was asked at all".
TEST_F(MigrateAsk, re_placing_a_thread_no_core_can_pick_asks_nobody)
{
    Placed p = place();
    ASSERT_EQ(owed_at(CORE_PEER), 0) << "fixture: nothing stands against the peer core";

    kernel().policy->on_remove(p.alt);
    p.alt->state = ThreadState::BLOCKED;
    p.alt->affinity = 1u << CORE_ME;

    sched::set_affinity(p.alt, 1u << CORE_PEER);
    EXPECT_EQ(owed_at(CORE_PEER), 0)
        << "a peer was asked to look for a thread parked on a wait queue. It will wake, find "
           "the ready set it already declined, and go back to what it was running; whatever "
           "makes the thread READY is what owes that core an ask";

    // Under the runner, so READY it waits behind strictly higher priority on this core.
    {
        IrqLock lock;
        sched::set_prio(p.alt, PRIO_UNDER_RUNNER);
    }
    p.alt->state = ThreadState::READY;
    kernel().policy->on_ready(p.alt);
    sched::set_affinity(p.alt, KICKOS_CORE_SET_ALL);
    EXPECT_NE(owed_at(CORE_PEER), 0)
        << "the SAME thread, now READY, waiting behind a higher priority here and eligible on "
           "the peer core, which runs below it, and that core was not told to look: without "
           "this half the arm above would pass for a set_affinity that asked nobody anything";
}

// The debug assertion that keeps ThreadState::RUNNING and the current[] seat one fact. Compiled
// out of a shipped image; live here, where KICKOS_DEBUG is on.
TEST_F(MigrateAsk, a_running_thread_no_core_seats_is_a_debug_assert)
{
    Placed p = place();
    ASSERT_EQ(p.runner->state, ThreadState::RUNNING) << "fixture: the thread is RUNNING";
    kernel().current[CORE_ME] = nullptr;

    KICKOS_EXPECT_PANIC(sched::set_affinity(p.runner, 1u << CORE_PEER),
                        "debug assert: seated == t");
}

TEST_F(MigrateAsk, a_thread_re_masked_off_a_peers_core_asks_that_peer)
{
    Placed p = place();
    ASSERT_EQ(owed_at(CORE_PEER), 0) << "fixture: nothing stands against the peer core";

    sched::set_affinity(p.below, 1u << CORE_ME);

    ASSERT_EQ(kernel().current[CORE_ME], p.runner) << "fixture: this core took no switch";
    ASSERT_EQ(kernel().current[CORE_PEER], p.below)
        << "fixture: no core may pull a thread off another's CPU";
    EXPECT_NE(owed_at(CORE_PEER), 0)
        << "the thread is excluded from the only core it executes on, and that core was not "
           "asked: the mask is published against a core that has no reason to take a pass, "
           "and the thread keeps running where it is no longer allowed to";
}

// A wake this core answers itself still owes a peer an ask, for the thread the answer cost
// this core. The woken thread never entered a peer's reach; the displaced one did.
TEST_F(MigrateAsk, a_wake_this_core_takes_itself_asks_for_the_thread_it_displaces)
{
    Placed p = place();
    Thread* const woken = seat_pool(3, PRIO_ABOVE);
    park_join(woken, p.runner);
    drain(CORE_ME);
    drain(CORE_PEER);
    ASSERT_EQ(owed_at(CORE_PEER), 0) << "fixture: nothing stands against the peer core";

    {
        IrqLock lock;
        sched::wake(woken);
    }

    ASSERT_EQ(kernel().current[CORE_ME], woken)
        << "fixture: the woken thread outranks this core's and is placeable here, so this "
           "core is the one that takes it";
    ASSERT_EQ(p.runner->state, ThreadState::HANDED)
        << "fixture: taking the woken thread cost this core the thread it was running, and "
           "that thread was handed to the peer";
    EXPECT_NE(owed_at(CORE_PEER), 0)
        << "the displaced thread is READY, placeable on the peer core, and outranks what that "
           "core runs, and nobody told it: the peer keeps a lower-priority thread on the CPU "
           "while a higher-priority one sits ready, and no later event is owed that would end "
           "it";
}

// Which thread the ask names, and it is not the woken one. The peer is put between the two
// priorities, so an ask carrying the woken thread's reaches it and one carrying the displaced
// thread's does not.
TEST_F(MigrateAsk, the_ask_a_taken_wake_owes_is_bounded_by_the_displaced_threads_priority)
{
    Placed p = place();
    // Through set_prio: the peer's running thread sits in its own ready list at its priority,
    // and that list is what placement reads.
    {
        IrqLock lock;
        sched::set_prio(p.below, PRIO_BETWEEN);
    }
    Thread* const woken = seat_pool(3, PRIO_ABOVE);
    park_join(woken, p.runner);
    drain(CORE_ME);
    drain(CORE_PEER);

    {
        IrqLock lock;
        sched::wake(woken);
    }

    ASSERT_EQ(kernel().current[CORE_ME], woken) << "fixture: this core takes the woken thread";
    ASSERT_EQ(p.runner->state, ThreadState::READY) << "fixture: and displaces what it ran";
    EXPECT_EQ(owed_at(CORE_PEER), 0)
        << "a peer running ABOVE the displaced thread was asked to look. It would take the "
           "raise, contend the kernel lock and re-pick what it already runs: the woken "
           "thread's priority is not what this ask may carry";

    // The same shape with the peer put back below what the next wake displaces.
    {
        IrqLock lock;
        sched::set_prio(p.below, PRIO_BELOW);
    }
    Thread* const higher = seat_pool(4, PRIO_TOP);
    park_join(higher, woken);
    drain(CORE_ME);
    drain(CORE_PEER);

    {
        IrqLock lock;
        sched::wake(higher);
    }

    ASSERT_EQ(kernel().current[CORE_ME], higher) << "fixture: this core takes the woken thread";
    EXPECT_NE(owed_at(CORE_PEER), 0)
        << "the peer now runs below the displaced thread and was still not told: without this "
           "half the assertion above would pass for a wake that asked nobody anything";
}

// The outgoing thread of a taken wake is not always a thread that became available: a core
// whose current parked before the wake gives the ready set nothing, and the arm above is what
// keeps this one from passing for an ask that never fires at all.
TEST_F(MigrateAsk, a_wake_that_displaces_a_thread_which_parked_instead_asks_nobody)
{
    Placed p = place();
    Thread* const woken = seat_pool(3, PRIO_ABOVE);
    park_join(woken, p.alt);
    // The shape a rendezvous has: this core's thread parks, hands the CPU to the thread it
    // woke, and is on no ready list for any peer to find.
    park_join(p.runner, p.alt);
    drain(CORE_ME);
    drain(CORE_PEER);
    ASSERT_EQ(owed_at(CORE_PEER), 0) << "fixture: nothing stands against the peer core";

    wake_next_park(leave_parked);
    {
        IrqLock lock;
        sched::wake(woken);
    }

    ASSERT_EQ(kernel().current[CORE_ME], woken) << "fixture: this core takes the woken thread";
    ASSERT_EQ(p.runner->state, ThreadState::BLOCKED)
        << "fixture: the outgoing thread parked, so the switch stored nothing over it";
    EXPECT_EQ(owed_at(CORE_PEER), 0)
        << "a peer was asked to look for a thread parked on a wait queue: it would take the "
           "raise, contend the kernel lock, find the ready set it already declined and "
           "re-pick what it was running";
}

TEST_F(MigrateAsk, a_wake_this_core_cannot_place_still_asks_the_peer)
{
    Placed p = place();
    Thread* const woken = seat_pool(3, PRIO_ABOVE);
    woken->affinity = 1u << CORE_PEER;
    park_join(woken, p.runner);
    drain(CORE_ME);
    drain(CORE_PEER);
    ASSERT_EQ(owed_at(CORE_PEER), 0) << "fixture: nothing stands against the peer core";

    {
        IrqLock lock;
        sched::wake(woken);
    }

    ASSERT_EQ(kernel().current[CORE_ME], p.runner)
        << "fixture: this core may not run the woken thread, so its own pick stands";
    EXPECT_NE(owed_at(CORE_PEER), 0)
        << "the woken thread is eligible only on the peer core, and that core was not told "
           "to look: without this half the arm above would pass for a wake that asked nobody "
           "anything";
}
