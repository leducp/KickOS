// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// An IRQ binding remains valid while its server holds any CAP_WAIT alias.
// Test closing a SIGNAL alias, closing one of two WAIT aliases, closing the
// last WAIT alias, and thread teardown.

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

// Closing the doorbell capability must preserve the server's WAIT binding.
TEST_F(IrqBindOwn, closing_a_signal_alias_leaves_the_wait_binding_standing)
{
    Bound const b = bind_server();
    uint32_t const doorbell = alias(b.server, b.obj, CAP_SIGNAL);

    {
        IrqLock lock;
        ASSERT_EQ(handle_close(b.server, doorbell), 0);
    }

    EXPECT_EQ(bound_server(b.obj), b.server);
    // Verify that the remaining authority still allows delivery.
    EXPECT_EQ(irq_notify(b.server, b.claim), 0);
    EXPECT_EQ(b.server->notify_pending & b.bit, b.bit);
}

// Close the original claim before its WAIT alias. Keep a SIGNAL alias alive
// so the binding stays allocated: a null target then proves release rather
// than destruction of the whole object.
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

// Preserve an unconsumed event for the next server when the last WAIT cap closes.
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
    EXPECT_EQ(b.server->notify_pending & b.bit, b.bit);

    {
        IrqLock lock;
        ASSERT_EQ(handle_close(b.server, second), 0);
    }
    EXPECT_EQ(b.server->notify_pending & b.bit, 0u);
    EXPECT_EQ(kernel().irq_bindings.resolve(b.obj), nullptr);
}

// Teardown must release the binding after closing every alias.
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
