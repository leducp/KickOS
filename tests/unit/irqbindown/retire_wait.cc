// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A thread parked until a released IRQ binding's grace period ends is woken by the event that
// ends it: the dispatch holding it open leaves, and that core's next scheduling pass wakes the
// waiter. Nothing polls. Single-threaded: core one's dispatch entry runs the arm as core zero
// from inside its handler.

#include "../common/seam_pools.h"

#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/irq.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/notify.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <kickos/sys/errno.h>

#include <new>

#include "kseam_test.h"

using namespace kickos;
using namespace kickos::testfix;

namespace
{
    static_assert(KICKOS_KERNEL_CORES == 2, "core one's dispatch entry is the peer of core zero");

    constexpr int LINE_A = 5;
    constexpr int LINE_CARRIER = 7;
    constexpr uint8_t PRIO = 10;

    class IrqRetireWait : public KSeam
    {
    };

    Thread* g_holder = nullptr;
    Thread* g_waiter = nullptr;
    Task* g_record = nullptr;
    uint32_t g_line = KCAP_INVALID;
    uint32_t g_note = KCAP_INVALID;

    struct Inside
    {
        bool ran;
        bool retiring;
        uint32_t asked;
        bool parked_after_pass;
        uint32_t self_raises;
    };
    Inside g_inside;

    // Inside core one's dispatch entry: as core zero, the release and a waiter parked on its
    // grace period; then, as core one, a doorbell pass nested in that same dispatch.
    void release_and_park(void*)
    {
        g_core = 0;
        {
            IrqLock lock;
            (void)handle_close(g_holder, g_line, lock);
            (void)handle_close(g_holder, g_note, lock);
            irq_retire_drain();
            g_inside.retiring = task_irq_retiring(g_record)->head != nullptr;
            policy_on_remove(g_waiter);
            testfix::seat_blocked(g_waiter);
            g_waiter->wait_kind = WAIT_RETIRE;
            g_waiter->wait_result = WAIT_RESULT_POISON;
            g_ipi_send_mask = 0;
            irq_retire_await(g_waiter, lock);
            g_inside.asked = g_ipi_send_mask;
        }
        dispatch_as(1);
        g_inside.parked_after_pass = g_waiter->wait_kind == WAIT_RETIRE;
        g_inside.ran = true;
        g_core = 1;
        g_inside.self_raises = g_ipi_self_raises;
    }

    void seat()
    {
        g_holder = seat_pool(0, PRIO);
        g_record = task(0);
        join_task(g_holder, g_record);
        attach_caps(g_holder, KICKOS_SEAM_CAP_WIDTH);
        g_holder->affinity = 1u;
        g_waiter = seat_pool(1, PRIO);
        join_task(g_waiter, task(1));
        g_waiter->affinity = 1u;
        {
            IrqLock lock;
            sched::reschedule(nullptr, lock);
        }
        // Core one runs a thread of its own over an idle of its own, as every core with a
        // scheduler does; the fixture seats an idle for the boot core alone.
        alignas(Thread) static unsigned char idle_store[sizeof(Thread)];
        Thread* const peer_idle = new (idle_store) Thread{};
        peer_idle->base_prio = KICKOS_PRIO_IDLE;
        peer_idle->prio = KICKOS_PRIO_IDLE;
        sched::add_idle(peer_idle, core_at(1));
        Thread* const peer = seat_pool(2, PRIO);
        peer->affinity = 1u << 1;
        testfix::seat_running_on(peer, core_at(1));
        ASSERT_EQ(kernel().current(core_at(1)), peer) << "fixture: core one runs nothing";
        ASSERT_EQ(notify_create(g_holder, &g_note), 0);
        ASSERT_EQ(irq_claim(g_holder, LINE_A, 0, &g_line), 0);
        ASSERT_EQ(irq_bind_notify(g_holder, g_line, g_note), 0);
        ASSERT_TRUE(irq_attach(LINE_CARRIER, release_and_park, nullptr));
        g_inside = Inside{false, false, 0u, false, 0u};
    }
}

TEST_F(IrqRetireWait, the_dispatch_holding_a_grace_period_open_wakes_its_waiter_on_leaving)
{
    seat();
    g_core = 1;
    kickos_isr_irq(LINE_CARRIER);
    uint32_t const raised = g_ipi_self_raises - g_inside.self_raises;
    g_core = 0;
    ASSERT_TRUE(g_inside.ran) << "the handler never ran inside core one's dispatch entry";
    ASSERT_TRUE(g_inside.retiring) << "fixture: the released binding did not retire";
    EXPECT_EQ(g_inside.asked, 1u << 1) << "the park did not raise the core holding the grace open";
    EXPECT_TRUE(g_inside.parked_after_pass)
        << "a pass nested in the dispatch holding the grace period open woke the waiter";
    EXPECT_EQ(raised, 1u) << "the dispatch's exit did not raise its own core's pass";
    EXPECT_EQ(g_waiter->wait_kind, WAIT_RETIRE) << "the exit woke the waiter without a pass";
    dispatch_as(1);
    EXPECT_EQ(g_waiter->wait_kind, WAIT_NONE) << "the pass after the dispatch left the waiter parked";
    EXPECT_EQ(g_waiter->wait_result, 0);
    EXPECT_EQ(task_irq_retiring(g_record)->head, nullptr) << "and the binding was reclaimed";
    EXPECT_TRUE(kernel().retire_waiters.empty());
}

TEST_F(IrqRetireWait, a_cancel_takes_a_retire_waiter_off_the_waiters)
{
    seat();
    g_core = 1;
    kickos_isr_irq(LINE_CARRIER);
    g_core = 0;
    ASSERT_EQ(g_waiter->wait_kind, WAIT_RETIRE) << "fixture: the waiter is not parked";
    {
        IrqLock lock;
        thread_cancel_kind(g_waiter, CANCEL_KILL, lock);
    }
    EXPECT_EQ(g_waiter->wait_kind, WAIT_NONE);
    EXPECT_EQ(g_waiter->wait_result, -KOS_ECANCELED);
    EXPECT_TRUE(kernel().retire_waiters.empty());
}

namespace
{
    bool g_released = false;

    // Inside core one's dispatch entry, as core zero: the release and the drain that finds the
    // grace period held open by core one.
    void release_and_drain(void*)
    {
        g_core = 0;
        IrqLock lock;
        (void)handle_close(g_holder, g_line, lock);
        (void)handle_close(g_holder, g_note, lock);
        irq_retire_drain();
        g_released = task_irq_retiring(g_record)->head != nullptr;
        g_core = 1;
    }
}

// The holder leaves its dispatch between the waiter's drain and its park: no core holds the grace
// period any more, and the waiter's own core serves it.
TEST_F(IrqRetireWait, a_holder_leaving_between_the_drain_and_the_park_leaves_no_waiter_behind)
{
    seat();
    g_released = false;
    ASSERT_TRUE(irq_attach(LINE_CARRIER + 1, release_and_drain, nullptr));
    g_core = 1;
    kickos_isr_irq(LINE_CARRIER + 1);
    g_core = 0;
    ASSERT_TRUE(g_released) << "fixture: the drain inside the dispatch did not leave it retiring";
    {
        IrqLock lock;
        policy_on_remove(g_waiter);
        testfix::seat_blocked(g_waiter);
        g_waiter->wait_kind = WAIT_RETIRE;
        g_waiter->wait_result = WAIT_RESULT_POISON;
        irq_retire_await(g_waiter, lock);
    }
    dispatch_as(0);
    EXPECT_EQ(g_waiter->wait_kind, WAIT_NONE) << "nothing serves a waiter no core holds a grace for";
    EXPECT_EQ(g_waiter->wait_result, 0);
    EXPECT_EQ(task_irq_retiring(g_record)->head, nullptr);
    EXPECT_TRUE(kernel().retire_waiters.empty());
}

namespace
{
    constexpr int LINE_SECOND = LINE_CARRIER + 2;
    constexpr int LINE_PASS = LINE_CARRIER + 3;

    void nothing(void*)
    {
    }

    // Inside core one's dispatch, as core zero: the release, the park, and a second release whose
    // record waits behind the batch core one holds open.
    void release_park_and_release_again(void*)
    {
        release_and_park(nullptr);
        g_core = 0;
        irq_detach(LINE_SECOND);
        g_core = 1;
    }

    bool g_woken_in_pass = false;

    // Inside core zero's dispatch: the pass core one owes, taken once it has left its own.
    void core_one_pass(void*)
    {
        dispatch_as(1);
        g_woken_in_pass = g_waiter->wait_kind == WAIT_NONE;
    }
}

// The pass that ends the waiter's grace period closes the next batch on a core the waiter never
// asked: holding that grace is no reason to keep the waiter parked, or nothing wakes it.
TEST_F(IrqRetireWait, a_pass_that_reclaims_and_closes_a_batch_another_core_holds_wakes_the_waiter)
{
    seat();
    g_woken_in_pass = false;
    ASSERT_TRUE(irq_attach(LINE_SECOND, nothing, nullptr));
    ASSERT_TRUE(irq_attach(LINE_CARRIER + 1, release_park_and_release_again, nullptr));
    ASSERT_TRUE(irq_attach(LINE_PASS, core_one_pass, nullptr));
    g_core = 1;
    kickos_isr_irq(LINE_CARRIER + 1);
    g_core = 0;
    ASSERT_TRUE(g_inside.retiring) << "fixture: the released binding did not retire";
    ASSERT_EQ(g_inside.asked, 1u << 1) << "fixture: the park did not ask core one";
    ASSERT_EQ(g_waiter->wait_kind, WAIT_RETIRE) << "fixture: the waiter is not parked";
    kickos_isr_irq(LINE_PASS);
    EXPECT_TRUE(g_woken_in_pass) << "a pass ending the waiter's grace left it parked";
    EXPECT_EQ(g_waiter->wait_result, 0);
    EXPECT_EQ(task_irq_retiring(g_record)->head, nullptr);
    EXPECT_TRUE(kernel().retire_waiters.empty());
}
