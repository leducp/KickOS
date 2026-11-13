// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A released IRQ binding's grace period completes on its own above one core, whether or not
// anything waits on it: the release asks the core whose dispatch holds the batch open for a
// pass, that dispatch's exit raises it, and the pass reclaims. Nothing polls. Single-threaded:
// core one's dispatch entry runs the release as core zero from inside its handler.

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
#include <kickos/watch.h>

#include <kickos/sys/abi.h>
#include <kickos/sys/errno.h>

#include <new>

#include "kseam_test.h"

using namespace kickos;
using namespace kickos::testfix;

namespace
{
    uint32_t watch_spec(uint32_t kind, uint32_t events)
    {
        return (kind << KOS_WATCH_KIND_SHIFT) | events;
    }

    static_assert(KICKOS_KERNEL_CORES == 2, "core one's dispatch entry is the peer of core zero");

    constexpr int LINE_A = 5;
    constexpr int LINE_CARRIER = 7;
    constexpr uint8_t PRIO = 10;

    class IrqRetireDrive : public KSeam
    {
    };

    Thread* g_holder = nullptr;
    Task* g_record = nullptr;
    uint32_t g_line = KCAP_INVALID;
    uint32_t g_note = KCAP_INVALID;

    struct Inside
    {
        bool ran;
        bool retiring;
        uint32_t asked;
        bool retiring_after_pass;
        uint32_t self_raises;
    };
    Inside g_inside;

    // Inside core one's dispatch entry: as core zero, the release; then, as core one, a doorbell
    // pass nested in that same dispatch.
    void release_inside(void*)
    {
        g_core = 0;
        {
            IrqLock lock;
            g_ipi_send_mask = 0;
            (void)handle_close(g_holder, g_line, lock);
            (void)handle_close(g_holder, g_note, lock);
            g_inside.asked = g_ipi_send_mask;
            g_inside.retiring = task_irq_retiring(g_record)->head != nullptr;
        }
        dispatch_as(1);
        g_inside.retiring_after_pass = task_irq_retiring(g_record)->head != nullptr;
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
        ASSERT_EQ(watch_call(g_holder, watch_spec(KOS_WATCH_IRQ, KOS_IRQ_FIRED), g_line, g_note, 0, nullptr), 0);
        ASSERT_TRUE(irq_attach(LINE_CARRIER, release_inside, nullptr));
        g_inside = Inside{false, false, 0u, false, 0u};
    }
}

// Nothing waits on the grace period, and it still ends: the release asks core one, a pass nested
// in its dispatch leaves the batch open, the dispatch's exit raises its own pass, and that pass
// reclaims the binding and its payer's charge.
TEST_F(IrqRetireDrive, the_dispatch_holding_a_grace_period_open_is_asked_and_its_next_pass_reclaims)
{
    seat();
    g_core = 1;
    kickos_isr_irq(LINE_CARRIER);
    uint32_t const raised = g_ipi_self_raises - g_inside.self_raises;
    g_core = 0;
    ASSERT_TRUE(g_inside.ran) << "the handler never ran inside core one's dispatch entry";
    ASSERT_TRUE(g_inside.retiring) << "fixture: the released binding did not retire";
    EXPECT_EQ(g_inside.asked, 1u << 1) << "the release did not ask the core holding the grace open";
    EXPECT_TRUE(g_inside.retiring_after_pass)
        << "a pass nested in the dispatch holding the grace period open reclaimed it";
    EXPECT_EQ(raised, 1u) << "the dispatch's exit did not raise its own core's pass";
    EXPECT_NE(task_irq_retiring(g_record)->head, nullptr) << "the exit reclaimed without a pass";
    dispatch_as(1);
    EXPECT_EQ(task_irq_retiring(g_record)->head, nullptr) << "the pass after the dispatch left it";
    IrqLock lock;
    EXPECT_FALSE(irq_line_retiring(LINE_A));
}

namespace
{
    bool g_released = false;

    // Inside core one's dispatch entry, as core zero: the release, which finds the grace period
    // held open by core one.
    void release_only(void*)
    {
        g_core = 0;
        {
            IrqLock lock;
            (void)handle_close(g_holder, g_line, lock);
            (void)handle_close(g_holder, g_note, lock);
            g_released = task_irq_retiring(g_record)->head != nullptr;
        }
        g_core = 1;
    }
}

// The holder leaves its dispatch before any pass runs: the next drain on any core reclaims, and
// the reclamation owes that core a pass of its own.
TEST_F(IrqRetireDrive, a_holder_that_left_its_dispatch_leaves_the_batch_to_the_next_drain)
{
    seat();
    g_released = false;
    ASSERT_TRUE(irq_attach(LINE_CARRIER + 1, release_only, nullptr));
    g_core = 1;
    kickos_isr_irq(LINE_CARRIER + 1);
    g_core = 0;
    ASSERT_TRUE(g_released) << "fixture: the release inside the dispatch did not leave it retiring";
    uint32_t const raises = g_ipi_self_raises;
    {
        IrqLock lock;
        irq_retire_drain();
    }
    EXPECT_EQ(task_irq_retiring(g_record)->head, nullptr) << "a drain past the grace left it";
    EXPECT_GT(g_ipi_self_raises, raises) << "the reclamation owed its core no pass";
}

namespace
{
    constexpr int LINE_SECOND = LINE_CARRIER + 2;
    constexpr int LINE_PASS = LINE_CARRIER + 3;

    void nothing(void*)
    {
    }

    // Inside core one's dispatch, as core zero: the release, and a second release whose record
    // waits behind the batch core one holds open.
    void release_and_release_again(void*)
    {
        release_inside(nullptr);
        g_core = 0;
        irq_detach(LINE_SECOND);
        g_core = 1;
    }

    uint32_t g_asked_in_pass = 0;

    // Inside core zero's dispatch: the pass core one owes, taken once it has left its own.
    void core_one_pass(void*)
    {
        g_ipi_send_mask = 0;
        dispatch_as(1);
        g_asked_in_pass = g_ipi_send_mask;
    }
}

// The pass that ends one batch closes the next on a core inside a dispatch: it asks that core in
// turn, so the second batch completes too with nothing waiting on it.
TEST_F(IrqRetireDrive, a_pass_that_closes_a_batch_another_core_holds_asks_that_core)
{
    seat();
    g_asked_in_pass = 0;
    ASSERT_TRUE(irq_attach(LINE_SECOND, nothing, nullptr));
    ASSERT_TRUE(irq_attach(LINE_CARRIER + 1, release_and_release_again, nullptr));
    ASSERT_TRUE(irq_attach(LINE_PASS, core_one_pass, nullptr));
    g_core = 1;
    kickos_isr_irq(LINE_CARRIER + 1);
    g_core = 0;
    ASSERT_TRUE(g_inside.retiring) << "fixture: the released binding did not retire";
    {
        IrqLock lock;
        ASSERT_TRUE(irq_line_retiring(LINE_SECOND)) << "fixture: the second line is not retiring";
    }
    kickos_isr_irq(LINE_PASS);
    EXPECT_EQ(task_irq_retiring(g_record)->head, nullptr) << "the pass left the first batch";
    EXPECT_EQ(g_asked_in_pass, 1u << 0) << "the second batch did not ask the core holding it";
    {
        IrqLock lock;
        EXPECT_TRUE(irq_line_retiring(LINE_SECOND)) << "core zero's dispatch did not hold the batch";
    }
    dispatch_as(0);
    IrqLock lock;
    EXPECT_FALSE(irq_line_retiring(LINE_SECOND)) << "the asked core's pass left the second batch";
}

namespace
{
    constexpr int LINE_INNER = LINE_CARRIER + 4;
    constexpr int LINE_OUTER = LINE_CARRIER + 5;
    constexpr int ROUNDS = 8;

    bool g_held_by_both = false;

    // Inside core zero's dispatch, itself inside core one's: the release, so the batch waits out
    // both, then a pass on each core in turn while both still hold it.
    void release_and_bounce(void*)
    {
        {
            IrqLock lock;
            (void)handle_close(g_holder, g_line, lock);
            (void)handle_close(g_holder, g_note, lock);
            g_held_by_both = irq_line_retiring(LINE_A);
        }
        for (int round = 0; round < ROUNDS; round++)
        {
            dispatch_as(1);
            dispatch_as(0);
        }
    }

    void enter_core_zero(void*)
    {
        g_core = 0;
        kickos_isr_irq(LINE_INNER);
        g_core = 1;
    }
}

// Two cores inside the dispatches one batch waits out: each is asked once, and the passes they
// take do not ask each other back and forth while both stay inside. The batch still completes once
// both have left.
TEST_F(IrqRetireDrive, two_cores_holding_one_batch_are_each_asked_once)
{
    seat();
    g_held_by_both = false;
    ASSERT_TRUE(irq_attach(LINE_INNER, release_and_bounce, nullptr));
    ASSERT_TRUE(irq_attach(LINE_OUTER, enter_core_zero, nullptr));
    uint32_t const sends = g_ipi_sends;
    g_core = 1;
    kickos_isr_irq(LINE_OUTER);
    g_core = 0;
    ASSERT_TRUE(g_held_by_both) << "fixture: the released binding did not retire";
    EXPECT_LE(g_ipi_sends - sends, 2u) << "the holders asked each other at every pass";
    dispatch_as(0);
    dispatch_as(1);
    IrqLock lock;
    EXPECT_FALSE(irq_line_retiring(LINE_A)) << "the batch never completed";
}
