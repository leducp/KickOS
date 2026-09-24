// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A line is delivered on the core that claimed it, and a thread handling it does not migrate.
// Three refusals keep it so:
//   the claim   a claimer whose OWN mask is not exactly the core it runs on is refused.
//   the bind    one notification's lines share one claim core, so a line claimed on another
//               core is refused. The fixture's arch_irq_line_core is the GIC answer, no owner,
//               so a bind that reads the seam instead of the claim refuses nothing here.
//   the wait    a waiter whose OWN mask is not exactly the claim core is refused, whatever its
//               task's grant says, a mask merely containing that core included. Every thread
//               here belongs to no task, whose grant is the whole machine, so an admission that
//               reads the grant instead of the mask admits all.
// Each admission and the operation it guards share one lock scope: a task-mate re-masking the
// caller at the first lock release must find the operation already done.
// And a raise a core takes for a line claimed on another wakes nobody and masks nothing: every
// raise of a claimed line lands on its claim core, so that one was held from before the claim.

#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/irq.h>
#include <kickos/irqlock.h>
#include <kickos/notify.h>
#include <kickos/sched.h>
#include <kickos/thread.h>

#include <kickos/sys/errno.h>

#include "kseam_test.h"

using namespace kickos;
using namespace kickos::testfix;

namespace
{
    constexpr uint32_t CORE_A = 0;
    constexpr uint32_t CORE_B = 1;
    constexpr int LINE_ONE = 5;
    constexpr int LINE_TWO = 6;
    constexpr uint8_t PRIO_DRIVER = 10;

    class IrqLineCore : public KSeam
    {
    };

    // Left blocked, so a mask written here meets no ready queue the fixture would have to re-seat.
    Thread* driver()
    {
        Thread* const t = seat_pool(0, PRIO_DRIVER);
        attach_caps(t, KICKOS_CAP_CHILD_WIDTH);
        {
            IrqLock lock;
            kernel().policy->on_remove(t);
            t->state = ThreadState::BLOCKED;
        }
        return t;
    }

    constexpr uint32_t BOTH = (1u << CORE_A) | (1u << CORE_B);

    // The claim as `t` running on `core` with its mask set to `mask`.
    int claim_as(Thread* t, int line, uint32_t core, uint32_t mask, uint32_t* cap)
    {
        uint32_t const was = g_core;
        g_core = core;
        t->affinity = mask;
        *cap = KCAP_INVALID;
        int const rc = irq_claim(t, line, 0, cap);
        g_core = was;
        return rc;
    }

    uint32_t claim_on(Thread* t, int line, uint32_t core)
    {
        uint32_t cap = KCAP_INVALID;
        EXPECT_EQ(claim_as(t, line, core, 1u << core, &cap), 0);
        return cap;
    }

    IrqBinding const* binding_of(Thread* t, uint32_t cap)
    {
        IrqLock lock;
        CapEntry const* const e = cap_lookup(t, cap);
        if (e == nullptr)
        {
            return nullptr;
        }
        return kernel().irq_bindings.resolve(e->obj);
    }

    struct Chained
    {
        Thread* t;
        uint32_t note;
        uint32_t line; // LINE_ONE's capability, claimed on CORE_A and bound to `note`
    };

    Chained chain_on_core_a()
    {
        Chained c{};
        c.t = driver();
        EXPECT_EQ(notify_create(c.t, &c.note), 0);
        c.line = claim_on(c.t, LINE_ONE, CORE_A);
        EXPECT_EQ(irq_bind_notify(c.t, c.line, c.note), 0);
        EXPECT_EQ(notify_bind(c.t, c.note), 0);
        return c;
    }

    constexpr uint32_t ALL_BITS = 0xFFFFFFFFu;

    uint32_t pending_of(Thread* t, uint32_t note)
    {
        IrqLock lock;
        CapEntry const* const e = cap_lookup(t, note);
        if (e == nullptr)
        {
            return 0;
        }
        Notification const* const n = kernel().notifies.resolve(e->obj);
        if (n == nullptr)
        {
            return 0;
        }
        return n->pending;
    }

    // The line's dispatch entered on `core`, as that core's first-level handler enters it.
    void raise_on(int line, uint32_t core)
    {
        uint32_t const was = g_core;
        g_core = core;
        kickos_isr_irq(line);
        g_core = was;
    }

    // A task-mate's kos_thread_set_affinity, landing at the first lock release inside the call,
    // and what the line showed at that instant.
    Thread* g_remask = nullptr;
    bool g_armed_at_gap = false;
    uint32_t g_clears_at_gap = 0;

    void remask_at_first_release()
    {
        g_armed_at_gap = g_line_armed[LINE_ONE];
        g_clears_at_gap = g_line_clears[LINE_ONE];
        g_remask->affinity = 1u << CORE_B;
    }

    void remask_at_first_release_of(Thread* t)
    {
        g_remask = t;
        g_armed_at_gap = false;
        g_clears_at_gap = 0;
        run_in_chunk_gap(remask_at_first_release, 1);
    }
}

TEST_F(IrqLineCore, a_bind_refuses_a_line_claimed_on_another_core)
{
    Chained const c = chain_on_core_a();
    uint32_t const other = claim_on(c.t, LINE_TWO, CORE_B);

    EXPECT_EQ(irq_bind_notify(c.t, other, c.note), -KOS_EPERM);
    IrqBinding const* const b = binding_of(c.t, other);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->notify, nullptr) << "a refused bind left the line attached";
}

// Catches a refusal of every second signaller, which the arm above cannot tell from the rule.
TEST_F(IrqLineCore, a_bind_admits_a_second_line_claimed_on_the_same_core)
{
    Chained const c = chain_on_core_a();
    uint32_t const same = claim_on(c.t, LINE_TWO, CORE_A);

    EXPECT_EQ(irq_bind_notify(c.t, same, c.note), 0);
}

// A released line leaves the chain, so a re-claim on another core is a new binding that binds
// afresh and moves the object's delivery core with it.
TEST_F(IrqLineCore, a_line_released_and_reclaimed_on_another_core_binds_afresh)
{
    Chained const c = chain_on_core_a();
    {
        IrqLock lock;
        ASSERT_EQ(handle_close(c.t, c.line), 0);
    }
    uint32_t const again = claim_on(c.t, LINE_ONE, CORE_B);
    ASSERT_EQ(irq_bind_notify(c.t, again, c.note), 0);

    uint32_t const stale = claim_on(c.t, LINE_TWO, CORE_A);
    EXPECT_EQ(irq_bind_notify(c.t, stale, c.note), -KOS_EPERM)
        << "the chain's core did not move with its only signaller";
}

// A raise already pending, so an admitted wait returns at once instead of parking.
TEST_F(IrqLineCore, a_wait_refuses_a_waiter_whose_own_mask_excludes_the_claim_core)
{
    Chained const c = chain_on_core_a();
    raise_on(LINE_ONE, CORE_A);
    c.t->affinity = 1u << CORE_B;

    uint32_t bits = 0;
    EXPECT_EQ(notify_wait(c.t, c.note, ALL_BITS, KOS_TIMEOUT_NONE, &bits), -KOS_EPERM);
    EXPECT_EQ(bits, 0u);
    EXPECT_EQ(pending_of(c.t, c.note), 1u) << "a refused wait consumed the raise";
    EXPECT_EQ(c.t->affinity, 1u << CORE_B) << "the admission rewrote the waiter's mask";
}

TEST_F(IrqLineCore, a_wait_admits_a_waiter_pinned_to_the_claim_core)
{
    Chained const c = chain_on_core_a();
    raise_on(LINE_ONE, CORE_A);

    c.t->affinity = 1u << CORE_A;
    uint32_t bits = 0;
    EXPECT_EQ(notify_wait(c.t, c.note, ALL_BITS, KOS_TIMEOUT_NONE, &bits), 0);
    EXPECT_EQ(bits, 1u);
}

TEST_F(IrqLineCore, a_wait_refuses_a_waiter_whose_mask_merely_contains_the_claim_core)
{
    Chained const c = chain_on_core_a();
    raise_on(LINE_ONE, CORE_A);

    c.t->affinity = BOTH;
    uint32_t bits = 0;
    EXPECT_EQ(notify_wait(c.t, c.note, ALL_BITS, KOS_TIMEOUT_NONE, &bits), -KOS_EPERM);
    EXPECT_EQ(pending_of(c.t, c.note), 1u) << "a refused wait consumed the raise";
    EXPECT_EQ(c.t->affinity, BOTH) << "the admission rewrote the waiter's mask";
}

TEST_F(IrqLineCore, a_wait_is_done_before_a_task_mate_can_re_mask_its_waiter)
{
    Chained const c = chain_on_core_a();
    raise_on(LINE_ONE, CORE_A);
    c.t->affinity = 1u << CORE_A;
    remask_at_first_release_of(c.t);

    uint32_t bits = 0;
    ASSERT_EQ(notify_wait(c.t, c.note, ALL_BITS, KOS_TIMEOUT_NONE, &bits), 0);
    EXPECT_EQ(c.t->affinity, 1u << CORE_B) << "fixture: the re-mask never landed";
    EXPECT_TRUE(g_armed_at_gap)
        << "the wait's admission dropped the lock before its rearm, so a waiter re-masked off "
           "the claim core in between was served";
}

TEST_F(IrqLineCore, a_claim_refuses_a_claimer_whose_mask_is_wider_than_its_core)
{
    Thread* const t = driver();
    uint32_t cap = KCAP_INVALID;
    EXPECT_EQ(claim_as(t, LINE_ONE, CORE_A, BOTH, &cap), -KOS_EPERM);
    EXPECT_EQ(cap, KCAP_INVALID);
    EXPECT_EQ(t->affinity, BOTH) << "the refusal rewrote the claimer's mask";
    EXPECT_EQ(claim_as(t, LINE_ONE, CORE_A, 1u << CORE_A, &cap), 0)
        << "the refusal left the line unclaimable";
}

// A one-core mask naming a core other than the one the claim runs on is a claimer about to
// move, and the line would follow the running core.
TEST_F(IrqLineCore, a_claim_refuses_a_claimer_pinned_to_another_core)
{
    Thread* const t = driver();
    uint32_t cap = KCAP_INVALID;
    EXPECT_EQ(claim_as(t, LINE_ONE, CORE_A, 1u << CORE_B, &cap), -KOS_EPERM);
}

TEST_F(IrqLineCore, a_claim_takes_a_claimer_pinned_to_its_core)
{
    Thread* const t = driver();
    uint32_t cap = KCAP_INVALID;
    ASSERT_EQ(claim_as(t, LINE_ONE, CORE_B, 1u << CORE_B, &cap), 0);
    IrqBinding const* const b = binding_of(t, cap);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->claim_core, CORE_B);
}

TEST_F(IrqLineCore, an_ack_and_a_discard_refuse_a_caller_not_pinned_to_the_claim_core)
{
    Chained const c = chain_on_core_a();

    c.t->affinity = 1u << CORE_B;
    EXPECT_EQ(irq_ack(c.t, c.line), -KOS_EPERM);
    EXPECT_EQ(irq_discard(c.t, c.line), -KOS_EPERM);

    c.t->affinity = BOTH;
    EXPECT_EQ(irq_ack(c.t, c.line), -KOS_EPERM);
    EXPECT_EQ(irq_discard(c.t, c.line), -KOS_EPERM);

    c.t->affinity = 1u << CORE_A;
    EXPECT_EQ(irq_ack(c.t, c.line), 0);
    EXPECT_EQ(irq_discard(c.t, c.line), 0);
}

TEST_F(IrqLineCore, an_ack_is_done_before_a_task_mate_can_re_mask_its_caller)
{
    Chained const c = chain_on_core_a();
    c.t->affinity = 1u << CORE_A;
    ASSERT_FALSE(g_line_armed[LINE_ONE]) << "fixture: a claim leaves the line masked";
    remask_at_first_release_of(c.t);

    ASSERT_EQ(irq_ack(c.t, c.line), 0);
    EXPECT_EQ(c.t->affinity, 1u << CORE_B) << "fixture: the re-mask never landed";
    EXPECT_TRUE(g_armed_at_gap)
        << "the ack's admission dropped the lock before its unmask, so a caller re-masked off "
           "the claim core in between was served";
}

TEST_F(IrqLineCore, a_discard_is_done_before_a_task_mate_can_re_mask_its_caller)
{
    Chained const c = chain_on_core_a();
    c.t->affinity = 1u << CORE_A;
    uint32_t const before = g_line_clears[LINE_ONE];
    remask_at_first_release_of(c.t);

    ASSERT_EQ(irq_discard(c.t, c.line), 0);
    EXPECT_EQ(c.t->affinity, 1u << CORE_B) << "fixture: the re-mask never landed";
    EXPECT_EQ(g_clears_at_gap, before + 1u)
        << "the discard's admission dropped the lock before its clear, so a caller re-masked "
           "off the claim core in between was served";
}

// The controller's enable may be global, so the stray raise must leave the owner's line armed.
TEST_F(IrqLineCore, a_raise_taken_off_the_claim_core_wakes_nobody_and_leaves_the_line_armed)
{
    Chained const c = chain_on_core_a();
    c.t->affinity = 1u << CORE_A;
    ASSERT_EQ(irq_ack(c.t, c.line), 0);
    ASSERT_TRUE(g_line_armed[LINE_ONE]);

    raise_on(LINE_ONE, CORE_B);
    EXPECT_EQ(pending_of(c.t, c.note), 0u) << "a raise off the claim core woke the owner";
    EXPECT_TRUE(g_line_armed[LINE_ONE]) << "a raise off the claim core masked the owner's line";

    raise_on(LINE_ONE, CORE_A);
    EXPECT_EQ(pending_of(c.t, c.note), 1u) << "the claim core's own raise was dropped";
    EXPECT_FALSE(g_line_armed[LINE_ONE]);
}

// The old owner's core still holds a raise when the line is claimed again elsewhere: a queued
// post, or one acknowledged and not yet dispatched.
TEST_F(IrqLineCore, a_raise_the_old_core_held_does_not_reach_a_reclaim_on_another_core)
{
    Chained const c = chain_on_core_a();
    {
        IrqLock lock;
        ASSERT_EQ(handle_close(c.t, c.line), 0);
    }
    uint32_t const again = claim_on(c.t, LINE_ONE, CORE_B);
    ASSERT_EQ(irq_bind_notify(c.t, again, c.note), 0);
    c.t->affinity = 1u << CORE_B;
    ASSERT_EQ(irq_ack(c.t, again), 0);

    raise_on(LINE_ONE, CORE_A);
    EXPECT_EQ(pending_of(c.t, c.note), 0u) << "the old core's raise woke the new owner";
    EXPECT_TRUE(g_line_armed[LINE_ONE]) << "the old core's raise masked the new owner's line";

    raise_on(LINE_ONE, CORE_B);
    EXPECT_EQ(pending_of(c.t, c.note), 1u);
}
