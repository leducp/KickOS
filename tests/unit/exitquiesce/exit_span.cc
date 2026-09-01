// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The window between a dying thread's EXITED state becoming visible and its frame being
// parked, at two kernel cores, over the real sched::exit_current and the real klock.cc.
//
// THREE CLAIMS, one arm each:
//   the span      the swap parks the outgoing frame with the kernel lock still held, so the
//                 release inside the swap is the first one after the EXITED store.
//   no gap        no release at all happens while the running thread is EXITED and no swap
//                 has parked it.
//   the key       ThreadPool::alloc reclaims exactly the state that store publishes, and
//                 nothing a dying thread carries before it.
//
// BOTH HALVES OF THE SWAP, because they release the lock from different places: an immediate
// swap from inside arch_switch, a deferred one from the exception epilogue the fixture stands
// in for at the next arch_idle_wait.

#include <kickos/instance.h>
#include <kickos/sched.h>
#include <kickos/thread.h>

#include "kseam_test.h"

using namespace kickos;
using namespace kickos::testfix;

namespace
{
    constexpr uint8_t PRIO_DYING = 5;

    // The dying thread, current and RUNNING, in a POOL slot so a reclaim can reach it, with a
    // peer live so the last-thread-out path does not end the arm. The setup switch is dropped
    // from the counters.
    Thread* dying_in_pool()
    {
        Thread* c = seat_pool(0, PRIO_DYING);
        seat_pool(1, PRIO_DYING - 1);
        sched::reschedule();
        EXPECT_EQ(kernel().current[arch_cpu_id()], c) << "fixture: the seated thread is current";
        g_switches = 0;
        g_parks_committed = 0;
        g_parks_without_lock = 0;
        g_exit_window_opened = 0;
        g_park_from = nullptr;
        g_park_from_state = ThreadState::INACTIVE;
        trace_reset();
        return c;
    }

    void expect_span_intact(Thread* dying)
    {
        EXPECT_EQ(g_parks_committed, 1u) << "the exit reached exactly one swap";
        EXPECT_EQ(g_parks_without_lock, 0u) << "the swap parked with the kernel lock held";
        EXPECT_EQ(g_exit_window_opened, 0u)
            << "the lock was released with an unparked EXITED thread running";
        EXPECT_EQ(g_park_from, dying) << "the swap that ended the span parked the dying thread";
        EXPECT_EQ(g_park_from_state, ThreadState::EXITED)
            << "the parked thread already carried the state a reclaim reads";
    }
}

class ExitQuiesce : public kickos::testfix::KSeam
{
};

TEST_F(ExitQuiesce, an_immediate_swap_parks_the_dying_frame_under_the_lock)
{
    Thread* c = dying_in_pool();
    set_swap_mode(SwapMode::IMMEDIATE);

    run_exit(0);

    expect_span_intact(c);
    EXPECT_FALSE(klock_held()) << "the swap's release is the end of the span";
}

TEST_F(ExitQuiesce, a_deferred_swap_parks_the_dying_frame_under_the_lock)
{
    Thread* c = dying_in_pool();
    set_swap_mode(SwapMode::DEFERRED);

    run_exit(0);

    expect_span_intact(c);
    EXPECT_FALSE(klock_held()) << "the swap's release is the end of the span";
}

// The waiter sweep runs between the EXITED store and the swap, and every wake it does takes
// the bracket again. A nested release there is the same window as a split bracket.
TEST_F(ExitQuiesce, the_waiter_sweep_takes_no_release_between_the_store_and_the_swap)
{
    Thread* c = dying_in_pool();
    Thread* joiner_low = seat_pool(2, PRIO_DYING + 1);
    Thread* joiner_high = seat_pool(3, PRIO_DYING + 4);
    park_join(joiner_low, c);
    park_join(joiner_high, c);
    set_swap_mode(SwapMode::DEFERRED);

    run_exit(0);

    EXPECT_NE(joiner_low->state, ThreadState::BLOCKED) << "fixture: the sweep woke this joiner";
    EXPECT_NE(joiner_high->state, ThreadState::BLOCKED) << "fixture: the sweep woke this joiner";
    expect_span_intact(c);
}

TEST_F(ExitQuiesce, a_reclaim_takes_the_slot_the_span_published)
{
    Thread* c = dying_in_pool();
    set_swap_mode(SwapMode::DEFERRED);

    run_exit(0);

    ASSERT_EQ(c->state, ThreadState::EXITED);
    uint16_t const gen_before = kernel().threads.gen[0];
    int const claimed = kernel().threads.alloc();
    EXPECT_EQ(claimed, 0) << "the published slot is the one a spawn reclaims";
    EXPECT_EQ(kernel().threads.gen[0], static_cast<uint16_t>(gen_before + 1))
        << "the reclaim burned a generation, which is what kills a stale handle";
}

// The state a dying thread carries BEFORE the store: RUNNING with `dying` set, which is what
// exit_current holds while cap_teardown unmasks between chunks. A reclaim keyed on anything
// wider than EXITED takes the slot here, where the thread is still on the CPU.
TEST_F(ExitQuiesce, a_reclaim_refuses_a_slot_the_span_has_not_published)
{
    Thread* c = dying_in_pool();
    c->dying = true;

    uint16_t const gen_before = kernel().threads.gen[0];
    int const claimed = kernel().threads.alloc();
    EXPECT_NE(claimed, 0) << "a thread still on the CPU may not have its slot handed out";
    EXPECT_EQ(kernel().threads.gen[0], gen_before) << "the refused slot kept its generation";
    EXPECT_EQ(c->state, ThreadState::RUNNING) << "fixture: the arm never published the store";
}
