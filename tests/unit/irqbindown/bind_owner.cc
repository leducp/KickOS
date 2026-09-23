// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Covers the three holders of a notification's reference, one arm each: every capability
// naming it, the bind, and each attached IRQ binding. The bind keeps its own reference
// instead of leaning on a capability, since that capability's own close would otherwise hand
// the object to a stranger.

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
    constexpr int LINE = 5;
    constexpr uint8_t PRIO_SERVER = 10;

    class IrqBindOwn : public KSeam
    {
    };

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

    // Add an alias and its object reference; cap_install does not increment it.
    uint32_t alias(Thread* t, int obj, CapType type, uint8_t rights)
    {
        IrqLock lock;
        uint32_t cap = KCAP_INVALID;
        EXPECT_TRUE(obj_ref_inc(type, obj, rights));
        EXPECT_EQ(cap_install(t, obj, type, rights, &cap), 0);
        return cap;
    }

    Thread* bound_thread(int obj)
    {
        Notification const* const n = kernel().notifies.resolve(obj);
        if (n == nullptr)
        {
            return nullptr;
        }
        return n->bound;
    }

    uint32_t pending_of(int obj)
    {
        Notification const* const n = kernel().notifies.resolve(obj);
        if (n == nullptr)
        {
            return 0;
        }
        return n->pending;
    }

    struct Bound
    {
        Thread* server;
        uint32_t note;  // the full-rights capability notify_create handed out
        uint32_t claim; // the CAP_IRQ for LINE
        int obj;        // the notification's global handle
    };

    // A server holding a notification it is bound to, plus a claimed line attached to it on
    // bit 0. The line's own capability stays open: the arms below decide when it closes.
    Bound bind_server()
    {
        Bound b{};
        b.server = seat_pool(0, PRIO_SERVER);
        attach_caps(b.server, KICKOS_CAP_CHILD_WIDTH);
        {
            IrqLock lock;
            sched::reschedule();
        }
        EXPECT_EQ(notify_create(b.server, &b.note), 0);
        b.obj = obj_of(b.server, b.note);
        EXPECT_GE(b.obj, 0);
        EXPECT_EQ(notify_bind(b.server, b.note), 0);
        EXPECT_EQ(bound_thread(b.obj), b.server);
        EXPECT_EQ(irq_claim(b.server, LINE, 0, &b.claim), 0);
        EXPECT_EQ(irq_bind_notify(b.server, b.claim, b.note), 0);
        return b;
    }
}

// A SIGNAL copy is one holder among several: closing it neither ends the binding nor takes
// the object away.
TEST_F(IrqBindOwn, closing_a_signal_alias_leaves_the_binding_standing)
{
    Bound const b = bind_server();
    uint32_t const doorbell = alias(b.server, b.obj, CapType::CAP_NOTIFY, CAP_SIGNAL);

    {
        IrqLock lock;
        ASSERT_EQ(handle_close(b.server, doorbell), 0);
    }

    EXPECT_EQ(bound_thread(b.obj), b.server);
    // The remaining authority still delivers.
    EXPECT_EQ(notify_signal(b.server, b.note), 0);
    EXPECT_EQ(pending_of(b.obj) & 1u, 1u);
}

// The bind holds a reference of its own: closing every capability while a thread is bound
// must not free the object, since it is still named by that thread's TCB.
TEST_F(IrqBindOwn, the_last_capability_closing_under_a_live_bind_frees_nothing)
{
    Bound const b = bind_server();

    {
        IrqLock lock;
        ASSERT_EQ(handle_close(b.server, b.note), 0);
    }

    ASSERT_NE(kernel().notifies.resolve(b.obj), nullptr) << "the slot went away under a bind";
    EXPECT_EQ(bound_thread(b.obj), b.server);
    EXPECT_EQ(notify_bound_handle(b.server->notify_bound), b.obj);
}

// An unconsumed raise is left for the next server when the bound thread dies: the bits were
// never in the TCB.
TEST_F(IrqBindOwn, a_bound_thread_s_death_clears_the_binding_and_leaves_the_pending)
{
    Bound const b = bind_server();
    // A second holder, so the object survives the dying thread's own capability sweep and
    // the arm can read what that sweep left.
    Thread* const keeper = seat_pool(1, PRIO_SERVER);
    attach_caps(keeper, KICKOS_CAP_CHILD_WIDTH);
    (void)alias(keeper, b.obj, CapType::CAP_NOTIFY, CAP_WAIT);

    ASSERT_EQ(notify_signal(b.server, b.note), 0);
    ASSERT_EQ(pending_of(b.obj) & 1u, 1u);

    {
        IrqLock lock;
        b.server->dying = true;
        notify_unbind_self(b.server);
    }
    cap_teardown(b.server);

    ASSERT_NE(kernel().notifies.resolve(b.obj), nullptr) << "the keeper's reference was lost";
    EXPECT_EQ(bound_thread(b.obj), nullptr);
    EXPECT_EQ(b.server->notify_bound, KOS_NOTIFY_UNBOUND);
    EXPECT_EQ(pending_of(b.obj) & 1u, 1u) << "the next server's event was discarded";
}

// An attached IRQ binding is the third holder: with the bind gone and every capability
// closed, the line's own reference is what is left, and it goes with the line.
TEST_F(IrqBindOwn, the_attached_line_holds_the_last_reference_and_releases_it_at_its_own_death)
{
    Bound const b = bind_server();

    {
        IrqLock lock;
        ASSERT_EQ(notify_unbind(b.server, b.note), 0);
        ASSERT_EQ(handle_close(b.server, b.note), 0);
    }
    ASSERT_NE(kernel().notifies.resolve(b.obj), nullptr)
        << "the attached line's reference was never taken";

    {
        IrqLock lock;
        ASSERT_EQ(handle_close(b.server, b.claim), 0);
    }
    EXPECT_EQ(kernel().irq_bindings.resolve(obj_of(b.server, b.claim)), nullptr);
    EXPECT_EQ(kernel().notifies.resolve(b.obj), nullptr) << "the object outlived every holder";
}

// Teardown releases both kinds through the sweep, with the bind released ahead of it.
TEST_F(IrqBindOwn, teardown_releases_the_object_through_the_last_holder_it_closes)
{
    Bound const b = bind_server();
    (void)alias(b.server, b.obj, CapType::CAP_NOTIFY, CAP_WAIT);
    (void)alias(b.server, b.obj, CapType::CAP_NOTIFY, CAP_SIGNAL);

    {
        IrqLock lock;
        b.server->dying = true;
        notify_unbind_self(b.server);
    }
    cap_teardown(b.server);

    EXPECT_EQ(kernel().notifies.resolve(b.obj), nullptr);
    EXPECT_EQ(b.server->cap_irq_live, 0);
    EXPECT_EQ(b.server->notify_bound, KOS_NOTIFY_UNBOUND);
}

// A second thread cannot take a binding another thread holds, and a thread already bound
// cannot take a second object: the TCB names exactly one.
TEST_F(IrqBindOwn, a_bind_is_refused_both_ways_round)
{
    Bound const b = bind_server();
    Thread* const other = seat_pool(1, PRIO_SERVER);
    attach_caps(other, KICKOS_CAP_CHILD_WIDTH);
    uint32_t const theirs = alias(other, b.obj, CapType::CAP_NOTIFY, CAP_WAIT);
    EXPECT_EQ(notify_bind(other, theirs), -KOS_EBUSY);

    uint32_t second = KCAP_INVALID;
    ASSERT_EQ(notify_create(b.server, &second), 0);
    EXPECT_EQ(notify_bind(b.server, second), -KOS_EBUSY);
    // Idempotent on the object it already holds, and it must not take a second reference.
    EXPECT_EQ(notify_bind(b.server, b.note), 0);
}

// The release masks the line before it drops the route: the seam requires the line masked
// across the call.
TEST_F(IrqBindOwn, the_last_close_masks_the_line_before_it_drops_the_route)
{
    Bound const b = bind_server();
    ASSERT_EQ(irq_ack(b.server, b.claim), 0);
    ASSERT_TRUE(g_line_armed[LINE]) << "the ack left the line masked, so nothing below is tested";
    g_routed_armed = -1;

    {
        IrqLock lock;
        ASSERT_EQ(handle_close(b.server, b.claim), 0);
    }

    EXPECT_EQ(g_routed_armed, 0) << "the route was dropped while the line was still armed";
    EXPECT_FALSE(g_line_armed[LINE]) << "the release left the line armed";
}
