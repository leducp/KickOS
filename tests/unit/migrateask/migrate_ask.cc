// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The cross-core ask a re-placement owes, at two kernel cores, over the real
// sched::set_affinity and the real switch_book.
//
// A thread the new mask excludes from the core it EXECUTES on is never yanked: it stays
// RUNNING until a switch stores READY, and every peer's pick_next refuses a RUNNING thread.
// So the ask that tells its new cores to look cannot be sent when the mask changes; it has
// to ride on the far side of that store. The ask is a counter before it is a doorbell
// (kernel/sync/klock.cc), and a counter is readable on the host, where the doorbell is not.
//
// THREE LINES, one arm each:
//   the poke     switch_book's `if (migrated != nullptr) poke_peers_below(...)`, the ask that
//                follows the store making the thread takeable.
//   the guard    switch_book's `if (not sched_placeable_on(prev, kickos_kernel_core()))`,
//                which is what keeps every ordinary switch from paying for that ask.
//   the pass     set_affinity's `klock_resched_ask(1u << core)`, a different ask entirely:
//                the thread runs on a peer, and only that peer's own pass can move it.

#include <kickos/arch/arch.h>
#include <kickos/instance.h>
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
    constexpr uint8_t PRIO_BELOW = 3;

    // The ask cell is keyed by TARGET and read from the target's seat, so an arm asking
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

    // A peer running BELOW is what makes an ask expressible at all: poke_peers_below skips a
    // core already running at or above the thread's priority, and an arm arranged without one
    // asserts nothing.
    Placed place()
    {
        Placed p{};
        p.runner = seat_pool(0, PRIO_RUNNER);
        p.alt = seat_pool(1, PRIO_RUNNER);
        p.below = seat_pool(2, PRIO_BELOW);
        sched::reschedule();

        kernel().policy->on_remove(p.below);
        p.below->state = ThreadState::RUNNING;
        kernel().current[CORE_PEER] = p.below;

        drain(CORE_ME);
        drain(CORE_PEER);
        return p;
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
    ASSERT_EQ(p.runner->state, ThreadState::READY)
        << "fixture: that switch stored the state a peer's pick_next reads";
    EXPECT_NE(owed_at(CORE_PEER), 0)
        << "the thread is READY, eligible only on the peer core, and that core was never told "
           "to look: it runs a thread below and will keep running it until its next natural "
           "switch, so the re-placement is worth whatever the peer's timer happens to owe it";
}

TEST_F(MigrateAsk, a_mask_that_still_admits_the_core_asks_nobody_across_the_next_switch)
{
    Placed p = place();
    // Narrowed by hand, so the call below is a WIDENING that leaves the running core in.
    p.runner->affinity = 1u << CORE_ME;

    sched::set_affinity(p.runner, KICKOS_CORE_SET_ALL);
    ASSERT_EQ(kernel().current[CORE_ME], p.runner)
        << "fixture: a mask that still admits this core moves nothing";

    sched::yield();
    ASSERT_EQ(kernel().current[CORE_ME], p.alt) << "fixture: the yield took the switch";

    EXPECT_EQ(owed_at(CORE_PEER), 0)
        << "an ordinary switch asked a peer to reschedule. Nothing migrated: the outgoing "
           "thread is placeable where it already was, and the peer will wake, find the same "
           "ready set it declined, and go back to what it was running";
}

// A thread no core can PICK gets no ask, and the same thread READY does: the two halves are
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

    p.alt->state = ThreadState::READY;
    kernel().policy->on_ready(p.alt);
    sched::set_affinity(p.alt, KICKOS_CORE_SET_ALL);
    EXPECT_NE(owed_at(CORE_PEER), 0)
        << "the SAME thread, now READY and eligible on the peer core, and that core was not "
           "told to look: without this half the arm above would pass for a set_affinity that "
           "asked nobody anything";
}

// The debug assertion that keeps ThreadState::RUNNING and the current[] seat one fact. Compiled
// out of a shipped image; live here, where KICKOS_DEBUG is on.
TEST_F(MigrateAsk, a_running_thread_no_core_seats_is_a_debug_assert)
{
    Placed p = place();
    ASSERT_EQ(p.runner->state, ThreadState::RUNNING) << "fixture: the thread is RUNNING";
    kernel().current[CORE_ME] = nullptr;

    KICKOS_EXPECT_PANIC(sched::set_affinity(p.runner, 1u << CORE_PEER),
                        "debug assert: t->state != ThreadState::RUNNING");
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
