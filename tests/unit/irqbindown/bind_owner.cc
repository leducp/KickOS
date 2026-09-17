// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// WHO OWNS AN IRQ NOTIFICATION BINDING, which is the question a capability close has to ask
// before it revokes one. irq_notify_bind demands CAP_WAIT, so the authority behind a binding
// is the SET of WAIT-bearing capabilities the bound thread holds on that line, and one thread
// legitimately holds several aliases of one line through separate grants: a UART driver is
// handed the line with CAP_WAIT and the doorbell with CAP_SIGNAL.
//
// The arms below fix the three answers that set can give a close.
//
//   an alias remains    the binding stands, and a later post still lands in the word. This is
//                       the arm a close that revoked unconditionally fails: the closer walks
//                       away without the authority it never gave up, its next wait answers
//                       EPERM, and a post piles up in the latch with nobody to take it.
//   the last one goes   the binding is released, and a post then finds no target. Without
//                       this arm the one above passes over a close that revokes NOTHING,
//                       which leaves a pointer into a TCB the pool can reuse.
//   the thread dies     teardown releases it whatever the alias count was, because the sweep
//                       closes every entry and the last of them is a close of the third kind.
//
// A SECOND WAIT ALIAS, NOT A SIGNAL ONE, IS WHAT SEPARATES THE FIRST TWO. Testing the closed
// capability's own rights would answer both the same way and leave the multiple-WAIT case
// deciding by which alias the thread happened to close first.

#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/irq.h>
#include <kickos/irqlock.h>
#include <kickos/sched.h>
#include <kickos/thread.h>

#include <kickos/sys/errno.h>

#include "kseam_test.h"

using namespace kickos;
using namespace kickos::testfix;

namespace
{
    constexpr int LINE = 5;
    constexpr uint8_t PRIO_SERVER = 10;

    class IrqBindOwn : public KSeam
    {
    };

    // The claim's own object handle, which is what a close is keyed on and what an alias must
    // name to be an alias at all.
    int obj_of(Thread* t, uint32_t cap)
    {
        IrqLock lock;
        CapEntry const* const e = cap_lookup(t, cap);
        if (e == nullptr)
        {
            return -1;
        }
        return e->obj;
    }

    // A second capability on the same binding, taking the object reference the delegation
    // path takes for one: cap_install seats the entry and moves no counter of its own.
    uint32_t alias(Thread* t, int obj, uint8_t rights)
    {
        IrqLock lock;
        uint32_t cap = KCAP_INVALID;
        EXPECT_TRUE(obj_ref_inc(CapType::CAP_IRQ, obj, rights));
        EXPECT_EQ(cap_install(t, obj, CapType::CAP_IRQ, rights, &cap), 0);
        return cap;
    }

    Thread* bound_server(int obj)
    {
        IrqBinding const* const b = kernel().irq_bindings.resolve(obj);
        if (b == nullptr)
        {
            return nullptr;
        }
        return b->notify_target;
    }

    // A server holding the claim, bound, with the bit its word takes a post in.
    struct Bound
    {
        Thread* server;
        uint32_t claim;
        int obj;
        uint32_t bit;
    };

    Bound bind_server()
    {
        Bound b{};
        b.server = seat_pool(0, PRIO_SERVER);
        attach_caps(b.server, KICKOS_CAP_CHILD_WIDTH);
        {
            IrqLock lock;
            sched::reschedule();
        }
        EXPECT_EQ(irq_claim(b.server, LINE, 0, &b.claim), 0);
        EXPECT_EQ(irq_notify_bind(b.server, b.claim, &b.bit), 0);
        b.obj = obj_of(b.server, b.claim);
        EXPECT_GE(b.obj, 0);
        EXPECT_EQ(bound_server(b.obj), b.server);
        return b;
    }
}

// The designed two-alias driver: bound through the line capability, closing the doorbell.
TEST_F(IrqBindOwn, closing_a_signal_alias_leaves_the_wait_binding_standing)
{
    Bound const b = bind_server();
    uint32_t const doorbell = alias(b.server, b.obj, CAP_SIGNAL);

    {
        IrqLock lock;
        ASSERT_EQ(handle_close(b.server, doorbell), 0);
    }

    EXPECT_EQ(bound_server(b.obj), b.server);
    // The authority is not merely recorded: a post still reaches the word, which is what the
    // driver's next wait returns on.
    EXPECT_EQ(irq_notify(b.server, b.claim), 0);
    EXPECT_EQ(b.server->notify_pending & b.bit, b.bit);
}

// Two WAIT aliases: the first close is not the release and the second is. Closing the CLAIM
// first is deliberate, the surviving alias then being the one the bind did not run through.
//
// THE UNCLOSED SIGNAL ALIAS IS THE INSTRUMENT, not part of the case: without a reference
// still standing the last close frees the pool slot, the binding stops resolving for that
// reason alone, and the arm reads a null target over a close that released nothing.
TEST_F(IrqBindOwn, the_binding_ends_with_the_last_wait_alias_and_not_before)
{
    Bound const b = bind_server();
    uint32_t const second = alias(b.server, b.obj, CAP_WAIT);
    (void)alias(b.server, b.obj, CAP_SIGNAL);

    {
        IrqLock lock;
        ASSERT_EQ(handle_close(b.server, b.claim), 0);
    }
    EXPECT_EQ(bound_server(b.obj), b.server);

    {
        IrqLock lock;
        ASSERT_EQ(handle_close(b.server, second), 0);
    }
    ASSERT_NE(kernel().irq_bindings.resolve(b.obj), nullptr) << "the slot went away instead";
    EXPECT_EQ(bound_server(b.obj), nullptr);
}

// A post arriving between the two closes above goes back where a post with no server goes,
// so the successor of a partially-closed driver is not left silent.
TEST_F(IrqBindOwn, a_post_taken_before_the_last_close_returns_to_the_latch)
{
    Bound const b = bind_server();
    uint32_t const second = alias(b.server, b.obj, CAP_WAIT);
    ASSERT_EQ(irq_notify(b.server, b.claim), 0);
    ASSERT_EQ(b.server->notify_pending & b.bit, b.bit);

    {
        IrqLock lock;
        ASSERT_EQ(handle_close(b.server, b.claim), 0);
    }
    // Still served, so the bit stays where it was taken.
    EXPECT_EQ(b.server->notify_pending & b.bit, b.bit);

    {
        IrqLock lock;
        ASSERT_EQ(handle_close(b.server, second), 0);
    }
    EXPECT_EQ(b.server->notify_pending & b.bit, 0u);
    EXPECT_EQ(kernel().irq_bindings.resolve(b.obj), nullptr);
}

// Death releases it whatever the count was: the sweep closes every entry, so the last one is
// an ordinary close with no alias left.
TEST_F(IrqBindOwn, teardown_releases_the_binding_through_the_last_alias_it_closes)
{
    Bound const b = bind_server();
    (void)alias(b.server, b.obj, CAP_WAIT);
    (void)alias(b.server, b.obj, CAP_SIGNAL);

    b.server->dying = true;
    cap_teardown(b.server);

    EXPECT_EQ(kernel().irq_bindings.resolve(b.obj), nullptr);
    EXPECT_EQ(b.server->cap_irq_live, 0);
}
