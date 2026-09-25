// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Two cores placing toward a third, at three kernel cores, over the real publish and the real
// placement read: the second placer counts what the first already sent.

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
    constexpr uint32_t CORE_A = 0;
    constexpr uint32_t CORE_B = 1;
    constexpr uint32_t CORE_IDLE = 2;
    static_assert(KICKOS_KERNEL_CORES == 3, "the arm names three distinct cores");

    constexpr uint8_t PRIO_RUNNER = 6;
    constexpr uint8_t PRIO_FIRST = 5;
    constexpr uint8_t PRIO_SECOND = 4;

    Thread* add_idle_on(int slot, uint32_t core)
    {
        Thread* const t = &g_fx.t[slot];
        t->base_prio = KICKOS_PRIO_IDLE;
        t->prio = KICKOS_PRIO_IDLE;
        t->id = static_cast<uint16_t>(slot + 1);
        sched::add_idle(t, core);
        return t;
    }

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
}

class SchedRing3 : public kickos::testfix::KSeam
{
};

TEST_F(SchedRing3, two_placements_from_two_cores_toward_one_idle_core_do_not_pile_up)
{
    Thread* const runner_a = seat_pool(1, PRIO_RUNNER);
    Thread* const runner_b = seat_pool(2, PRIO_RUNNER);
    runner_a->affinity = 1u << CORE_A;
    runner_b->affinity = 1u << CORE_B;
    Thread* const idle_b = add_idle_on(0, CORE_B);
    Thread* const idle_c = add_idle_on(1, CORE_IDLE);
    {
        IrqLock lock;
        sched::reschedule();
    }
    seat_running_on(idle_b, CORE_B);
    seat_running_on(runner_b, CORE_B);
    seat_running_on(idle_c, CORE_IDLE);
    ASSERT_EQ(kernel().current[CORE_A], runner_a) << "fixture: core A runs its runner";
    Thread* const first = seat_pool(3, PRIO_FIRST);
    park_join(first, runner_a);
    Thread* const second = seat_pool(4, PRIO_SECOND);
    park_join(second, runner_a);
    settle();

    wake_as(CORE_A, first);
    ASSERT_EQ(first->queue_core, CORE_IDLE) << "fixture: the first wake went to the idle core";
    ASSERT_EQ(first->state, ThreadState::HANDED) << "fixture: and is still on its way there";

    wake_as(CORE_B, second);

    EXPECT_EQ(second->queue_core, CORE_B)
        << "the second placer sent its thread to a core the first had already sent a higher "
           "one to: the idle core runs the first ahead of it, which the placement read as idle "
           "from the core's own level alone";
}

namespace
{
    constexpr uint32_t CORE_C = 2;
    constexpr uint8_t PRIO_UNDER = 5;
    constexpr uint8_t PRIO_HOG = 7;
    constexpr uint8_t PRIO_BOOST = 8;

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

    uint32_t in_flight(uint32_t from, uint32_t to)
    {
        return kernel().sched_out[from].staged[to] - kernel().sched_in[to].tail[from].load();
    }

    // A runner on A, a pinned hog running on each of B and C, and the thread READY on B behind
    // B's hog with every core in its mask.
    Thread* ready_on_b()
    {
        Thread* const runner = seat_pool(1, PRIO_RUNNER);
        runner->affinity = 1u << CORE_A;
        Thread* const idle_b = add_idle_on(0, CORE_B);
        Thread* const idle_c = add_idle_on(1, CORE_C);
        {
            IrqLock lock;
            sched::reschedule();
        }
        seat_running_on(idle_b, CORE_B);
        seat_running_on(idle_c, CORE_C);
        Thread* const hog_b = seat_pool(2, PRIO_HOG);
        seat_running_on(hog_b, CORE_B);
        hog_b->affinity = 1u << CORE_B;
        Thread* const hog_c = seat_pool(3, PRIO_HOG);
        seat_running_on(hog_c, CORE_C);
        hog_c->affinity = 1u << CORE_C;
        Thread* const t = seat_pool(4, PRIO_UNDER);
        park_join(t, runner);
        t->affinity = 1u << CORE_B;
        settle();
        wake_as(CORE_A, t);
        dispatch_as(CORE_B);
        settle();
        t->affinity = KICKOS_CORE_SET_ALL;
        return t;
    }

    // Request #1 is made by A toward B, and B hands the thread to C before its dispatch reads
    // it. A request A then makes is kept out of every ring by the slot byte #1 still holds.
    Thread* handed_to_c_behind_a_request(uint8_t first)
    {
        Thread* const t = ready_on_b();
        set_prio_as(CORE_A, t, first);
        set_affinity_as(CORE_B, t, 1u << CORE_C);
        return t;
    }
}

TEST_F(SchedRing3, a_boost_requested_after_a_move_follows_the_thread_to_a_third_core)
{
    Thread* const t = handed_to_c_behind_a_request(PRIO_UNDER + 1u);
    dispatch_as(CORE_C);
    ASSERT_TRUE(linked_on(t, CORE_C)) << "fixture: C linked the moved thread";
    ASSERT_EQ(t->reseat_owed, 1u) << "fixture: request #1 is still unretired toward B";

    set_prio_as(CORE_A, t, PRIO_BOOST);
    dispatch_as(CORE_B);

    EXPECT_EQ(in_flight(CORE_B, CORE_C), 1u)
        << "B dropped the entry for a thread linked on C, and the boost it suppressed reaches "
           "nobody";

    dispatch_as(CORE_C);

    EXPECT_EQ(t->rq_prio, PRIO_BOOST) << "the boost never reached the list the thread sits on";
    EXPECT_EQ(t->reseat_owed, 0u) << "the forwarded request left its slot owed";
    EXPECT_EQ(kernel().current[CORE_C], t) << "the boosted thread does not run over C's hog";
}

TEST_F(SchedRing3, a_narrowing_requested_after_a_move_follows_the_thread_to_a_third_core)
{
    Thread* const t = handed_to_c_behind_a_request(PRIO_UNDER + 1u);
    dispatch_as(CORE_C);
    ASSERT_TRUE(linked_on(t, CORE_C)) << "fixture: C linked the moved thread";
    ASSERT_EQ(t->reseat_owed, 1u) << "fixture: request #1 is still unretired toward B";

    set_affinity_as(CORE_A, t, 1u << CORE_B);
    dispatch_as(CORE_B);
    dispatch_as(CORE_C);
    dispatch_as(CORE_B);

    EXPECT_EQ(t->queue_core, CORE_B)
        << "the thread stays linked on a core its mask no longer names";
    EXPECT_TRUE(linked_on(t, CORE_B)) << "the core the mask names never linked it";
    EXPECT_EQ(t->reseat_owed, 0u) << "the forwarded request left its slot owed";
}

TEST_F(SchedRing3, a_slay_requested_after_a_move_follows_the_thread_to_a_third_core)
{
    Thread* const t = handed_to_c_behind_a_request(PRIO_BOOST);
    dispatch_as(CORE_C);
    ASSERT_EQ(kernel().current[CORE_C], t) << "fixture: the moved thread runs on C";
    ASSERT_EQ(t->reseat_owed, 1u) << "fixture: request #1 is still unretired toward B";
    g_redirect_target = nullptr;

    slay_as(CORE_A, t);
    dispatch_as(CORE_B);
    dispatch_as(CORE_C);

    EXPECT_NE(kernel().current[CORE_C], t)
        << "the slain thread keeps its core: B dropped the request for a thread running on C";

    dispatch_as(CORE_C);

    EXPECT_EQ(g_redirect_target, t) << "the pass C owed itself never claimed the victim";
}

TEST_F(SchedRing3, a_request_for_a_thread_still_handed_to_a_third_core_is_read_at_its_link)
{
    Thread* const t = handed_to_c_behind_a_request(PRIO_UNDER + 1u);
    ASSERT_EQ(t->state, ThreadState::HANDED) << "fixture: the thread is on its way to C";
    ASSERT_EQ(t->queue_core, CORE_C) << "fixture: the thread is on its way to C";

    set_prio_as(CORE_A, t, PRIO_BOOST);
    dispatch_as(CORE_B);

    EXPECT_EQ(t->reseat_owed, 0u) << "B kept the slot owed for a thread whose link is still ahead";
    EXPECT_EQ(in_flight(CORE_B, CORE_C), 1u) << "B sent C more than the handoff";

    dispatch_as(CORE_C);

    EXPECT_EQ(t->rq_prio, PRIO_BOOST) << "the link did not read the field the request wrote";
    EXPECT_EQ(kernel().current[CORE_C], t) << "the boosted thread does not run over C's hog";
}
