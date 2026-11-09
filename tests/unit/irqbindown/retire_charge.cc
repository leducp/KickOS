// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Above one kernel core a released IRQ binding keeps its slot until no core can still be inside
// a dispatch that read it, and the task that last held it pays for that slot, and for the
// notification it still raises into, until then, and no task is seated in that record before.
// Single-threaded: core one's dispatch entry runs the arm as core zero from inside its handler.

#include "../common/seam_pools.h"

#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/irq.h>
#include <kickos/irqlock.h>
#include <kickos/notify.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <kickos/sys/errno.h>

#include "kseam_test.h"

using namespace kickos;
using namespace kickos::testfix;

namespace
{
    static_assert(KICKOS_KERNEL_CORES == 2, "core one's dispatch entry is the peer of core zero");
    static_assert(KICKOS_SEAM_BUDGET_IRQ_BINDINGS == 1 and KICKOS_SEAM_BUDGET_NOTIFICATIONS == 1,
                  "the arms count one binding and one notification against the task by hand");
    static_assert(KICKOS_SEAM_IRQ_BINDINGS > 2 and KICKOS_SEAM_NOTIFY > 2,
                  "the pools have slots past the budgets, so a refusal is the budget's");

    constexpr int LINE_A = 5;
    constexpr int LINE_B = 6;
    constexpr int LINE_CARRIER = 7;
    constexpr uint8_t PRIO = 10;

    class IrqRetireCharge : public KSeam
    {
    };

    Thread* g_holder = nullptr;
    uint32_t g_line = KCAP_INVALID;
    uint32_t g_note = KCAP_INVALID;
    int g_claim_inside = 0;
    int g_create_inside = 0;
    bool g_ran = false;

    // Inside core one's dispatch entry, as core zero: the release, then a claim of another line
    // and a create of another notification while the released binding still retires.
    void carrier(void*)
    {
        g_core = 0;
        {
            IrqLock lock;
            (void)handle_close(g_holder, g_line, lock);
            (void)handle_close(g_holder, g_note, lock);
        }
        uint32_t other = KCAP_INVALID;
        g_claim_inside = irq_claim(g_holder, LINE_B, 0, &other);
        g_create_inside = notify_create(g_holder, &other);
        g_ran = true;
        g_core = 1;
    }

    struct Ended
    {
        int decl_state;
        bool listed;
    };
    Ended g_ended;

    // The holder's task record and the declaration it is.
    Task* g_record = nullptr;

    size_t decl_of(Task const* t)
    {
        return static_cast<size_t>(kernel().tasks().size() - 1 - (t - kernel().tasks().base));
    }

    // Inside core one's dispatch entry, as core zero: the release, then the end of the holder's
    // task, its last member leaving and its creator letting go, while the binding still retires.
    void carrier_then_the_task_ends(void*)
    {
        g_core = 0;
        IrqLock lock;
        (void)handle_close(g_holder, g_line, lock);
        (void)handle_close(g_holder, g_note, lock);
        (void)task_release(g_record, lock);
        task_member_leave(g_holder, kernel().threads.index_of(g_holder));
        (void)task_sweep_done(g_record);
        task_drop_hold(g_record, lock);
        g_ended.decl_state = task_decl_state(decl_of(g_record));
        g_ended.listed = kernel().task_free.holds(&g_record->free_node);
        g_ran = true;
        g_core = 1;
    }

    // A task's thread on core zero holding LINE_A, bound to a notification it holds.
    void seat_holder()
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
        ASSERT_EQ(notify_create(g_holder, &g_note), 0);
        ASSERT_EQ(irq_claim(g_holder, LINE_A, 0, &g_line), 0);
        ASSERT_EQ(irq_bind_notify(g_holder, g_line, g_note), 0);
        ASSERT_TRUE(irq_attach(LINE_CARRIER, carrier, nullptr));
        g_ran = false;
    }

    void peer_dispatch()
    {
        g_core = 1;
        kickos_isr_irq(LINE_CARRIER);
        g_core = 0;
    }
}

TEST_F(IrqRetireCharge, a_retiring_binding_is_charged_to_its_last_holder_until_reclaimed)
{
    seat_holder();
    peer_dispatch();
    ASSERT_TRUE(g_ran) << "the carrier handler never ran inside core one's dispatch entry";
    EXPECT_EQ(g_claim_inside, -KOS_EAGAIN)
        << "the released binding still holds its slot, so the task's claim of another line "
           "takes one past its budget";
    uint32_t other = KCAP_INVALID;
    EXPECT_EQ(irq_claim(g_holder, LINE_B, 0, &other), 0)
        << "once core one left its dispatch the slot returns, and the charge with it";
}

TEST_F(IrqRetireCharge, the_notification_a_retiring_binding_raises_into_stays_charged)
{
    seat_holder();
    peer_dispatch();
    ASSERT_TRUE(g_ran) << "the carrier handler never ran inside core one's dispatch entry";
    EXPECT_EQ(g_create_inside, -KOS_EAGAIN)
        << "the retiring binding still holds its notification, so another create takes one past "
           "the task's budget";
    uint32_t other = KCAP_INVALID;
    EXPECT_EQ(notify_create(g_holder, &other), 0)
        << "once core one left its dispatch the admission itself ends the retirement";
}

TEST_F(IrqRetireCharge, a_record_paying_for_a_retiring_binding_is_seated_again_only_once_it_is_reclaimed)
{
    seat_holder();
    g_ended = Ended{0, true};
    ASSERT_TRUE(irq_attach(LINE_CARRIER + 1, carrier_then_the_task_ends, nullptr));
    g_core = 1;
    kickos_isr_irq(LINE_CARRIER + 1);
    g_core = 0;
    ASSERT_TRUE(g_ran) << "the handler never ran inside core one's dispatch entry";
    EXPECT_EQ(g_ended.decl_state, -KOS_EAGAIN)
        << "the declaration's next create was free to seat the record under the retiring charge";
    EXPECT_FALSE(g_ended.listed) << "the record went back to the free list under the retiring charge";
    IrqLock lock;
    EXPECT_EQ(task_decl_state(decl_of(g_record)), 0)
        << "once core one left its dispatch the record is free again";
    EXPECT_TRUE(kernel().task_free.holds(&g_record->free_node));
}
