// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Test fused reply-receive with the real IPC code and a two-core scheduler.
// Every woken thread needs a peer notification, while only the highest-priority
// one is kept for the deferred local reschedule. Test both priority orders.
// Observe peer requests through kickos_kernel_core_resched_owed.
// Also check consumed-mask write-back after reply, flags, and buffer errors,
// and propagation of write-back failure.

#include <string.h>

#include <kickos/arch/arch.h>
#include <kickos/cap.h>
#include <kickos/endpoint.h>
#include <kickos/instance.h>
#include <kickos/irq.h>
#include <kickos/notify.h>
#include <kickos/irqlock.h>
#include <kickos/klock.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/thread.h>

#include <kickos/sys/abi.h>
#include <kickos/sys/errno.h>

#include "bench_seam.h"
#include "ipc_seam.h"
#include "kseam_test.h"
#include "syscall_internal.h"

using namespace kickos;
using namespace kickos::testfix;

namespace
{
    constexpr uint32_t CORE_ME = 0; // server and fixture core
    constexpr uint32_t CORE_PEER = 1; // peer core

    constexpr uint32_t ON_ME = 1u << CORE_ME;
    constexpr uint32_t ON_PEER = 1u << CORE_PEER;

    constexpr uint8_t PRIO_SERVER = 10;
    constexpr uint8_t PRIO_ABOVE_SERVER = 11; // preempts the server
    constexpr uint8_t PRIO_HIGH = 9;
    constexpr uint8_t PRIO_LOW = 8;
    constexpr uint8_t PRIO_PEER_RUNNING = 1; // lower than the threads being woken

    class FusedWake : public KSeam
    {
    };

    // Read the peer request using that core's fixture identity.
    bool owed_on(uint32_t core)
    {
        uint32_t const was = g_core;
        g_core = core;
        bool const owed = kickos_kernel_core_resched_owed() != 0;
        g_core = was;
        return owed;
    }

    // Clear requests produced by setup.
    void clear_peer_ask()
    {
        uint32_t const was = g_core;
        g_core = CORE_PEER;
        (void)kickos_kernel_core_resched_take();
        g_core = was;
    }

    // Server with a caller waiting for its reply and a queued plain sender.
    // Tests choose priorities and affinities to exercise both wake orders.
    struct Fused
    {
        Thread* server;
        Thread* answered; // parked on the reply cap, readied by the reply half
        Thread* sender;   // queued on the endpoint, readied by the receive half
        Thread* peer_running;
        uint32_t ep_cap;
        uint32_t reply_cap;
        uint8_t server_buf[8];
        uint8_t sender_buf[4];
    };

    void stage(Fused* f, uint8_t answered_prio, uint32_t answered_affinity, uint8_t sender_prio,
               uint32_t sender_affinity)
    {
        f->server = seat_pool(0, PRIO_SERVER);
        f->answered = seat_pool(1, answered_prio);
        f->sender = seat_pool(2, sender_prio);
        f->peer_running = seat_pool(3, PRIO_PEER_RUNNING);
        f->server->affinity = ON_ME;
        f->answered->affinity = answered_affinity;
        f->sender->affinity = sender_affinity;
        f->peer_running->affinity = ON_PEER;
        memset(f->server_buf, 0, sizeof(f->server_buf));
        for (size_t i = 0; i < sizeof(f->sender_buf); i++)
        {
            f->sender_buf[i] = static_cast<uint8_t>(i + 1);
        }
        attach_caps(f->server, KICKOS_CAP_CHILD_WIDTH);
        // Block the clients before scheduling so a higher-priority client cannot
        // run ahead of the server during setup.
        for (Thread* t : {f->answered, f->sender})
        {
            kernel().policy->on_remove(t);
            t->state = ThreadState::BLOCKED;
        }
        {
            IrqLock lock;
            sched::reschedule();
        }
        ASSERT_EQ(sched::current(), f->server);
        // Give the peer a running thread so it is eligible for reschedule requests.
        kernel().policy->on_remove(f->peer_running);
        f->peer_running->state = ThreadState::RUNNING;
        kickos::testfix::seat_running_on(f->peer_running, CORE_PEER);

        Endpoint* const ep = endpoint();
        {
            IrqLock lock;
            int const idx = kernel().endpoints.index_of(ep);
            ASSERT_EQ(cap_install(f->server, kernel().endpoints.handle_for(idx),
                                  CapType::CAP_ENDPOINT, CAP_WAIT, &f->ep_cap),
                      0);
            f->answered->call_state = CALL_REPLY_WAIT;
            ASSERT_EQ(cap_install_reply(f->server, f->answered, &f->reply_cap), 0);
            reply_donor_park(f->server, f->answered);
            // A plain send lets receive return immediately, exercising the guard
            // destructor instead of transferring its wake to a park.
            f->sender->ipc.buf = reinterpret_cast<uintptr_t>(f->sender_buf);
            f->sender->ipc.len = sizeof(f->sender_buf);
            f->sender->call_state = CALL_NONE;
            f->sender->wait_kind = WAIT_EP_SEND;
            f->sender->wait_obj = ep;
            f->sender->wait_queue = &ep->send_waiters;
            ep->send_waiters.push_back(&f->sender->link);
        }
        clear_peer_ask();
    }

    // Server bound to one IRQ, with no event pending.
    struct Line
    {
        Thread* server;
        uint32_t irq;
        uint32_t note;
        uint32_t bit;
    };

    // Use a real attached line and a real bind, so the kernel can rearm and consume its bit.
    void seat_server_with_a_line(Line* l)
    {
        l->server = seat_pool(0, PRIO_SERVER);
        // Pinned where it runs: a line is claimed and waited on only by a thread that cannot
        // migrate.
        l->server->affinity = ON_ME;
        attach_caps(l->server, KICKOS_CAP_CHILD_WIDTH);
        {
            IrqLock lock;
            sched::reschedule();
        }
        l->irq = KCAP_INVALID;
        l->note = KCAP_INVALID;
        l->bit = 1u << 0; // irq_bind_notify copies the capability's badge; unbadged is bit 0
        ASSERT_EQ(irq_claim(l->server, 5, 0, &l->irq), 0);
        ASSERT_EQ(notify_create(l->server, &l->note), 0);
        ASSERT_EQ(irq_bind_notify(l->server, l->irq, l->note), 0);
        ASSERT_EQ(notify_bind(l->server, l->note), 0);
        ASSERT_EQ(notify_pending_of(l->server), 0u);
    }

    // Combine a valid flag with an unknown bit to test unknown-bit rejection.
    constexpr uint32_t FLAGS_WITH_AN_UNDEFINED_BIT = KOS_RECV_NO_INFO | (KOS_RECV_NO_INFO << 1);

    int32_t serve(Fused* f)
    {
        kos_reply_recv_opts opts{};
        kos_reply_recv_opts_init(&opts, f->ep_cap, 0, KOS_TIMEOUT_NONE);
        return endpoint_reply_recv(f->reply_cap, reinterpret_cast<uintptr_t>(f->server_buf),
                                   kos_call_lens_pack(0, sizeof(f->server_buf)),
                                   reinterpret_cast<uintptr_t>(&opts));
    }
}

// The replied caller has higher priority. The sender can run only on the
// peer and still needs a reschedule request.
TEST_F(FusedWake, the_sender_behind_the_answered_caller_is_announced)
{
    Fused f{};
    ASSERT_NO_FATAL_FAILURE(stage(&f, PRIO_HIGH, ON_ME, PRIO_LOW, ON_PEER));

    ASSERT_EQ(serve(&f), static_cast<int32_t>(sizeof(f.sender_buf)));
    ASSERT_EQ(f.answered->wait_result, 0);
    ASSERT_EQ(f.sender->state, ThreadState::READY);
    EXPECT_TRUE(owed_on(CORE_PEER))
      << "the popped sender runs only on the peer, which was never asked";
}

// The sender has higher priority, so the replied caller needs a peer request.
TEST_F(FusedWake, the_answered_caller_the_sender_outranks_is_announced)
{
    Fused f{};
    ASSERT_NO_FATAL_FAILURE(stage(&f, PRIO_LOW, ON_PEER, PRIO_HIGH, ON_ME));

    ASSERT_EQ(serve(&f), static_cast<int32_t>(sizeof(f.sender_buf)));
    ASSERT_EQ(f.answered->state, ThreadState::READY);
    ASSERT_EQ(f.sender->state, ThreadState::READY);
    EXPECT_TRUE(owed_on(CORE_PEER))
      << "the answered caller runs only on the peer, which was never asked";
}

// Check both local selection of the highest-priority thread and notification
// of the peer for the other thread.
TEST_F(FusedWake, the_seat_goes_to_the_highest_while_the_peer_hears_about_the_rest)
{
    Fused f{};
    ASSERT_NO_FATAL_FAILURE(stage(&f, PRIO_ABOVE_SERVER, ON_ME, PRIO_LOW, ON_PEER));

    ASSERT_EQ(serve(&f), static_cast<int32_t>(sizeof(f.sender_buf)));
    EXPECT_EQ(sched::current(), f.answered) << "the deferred seat was not fed the highest";
    EXPECT_TRUE(owed_on(CORE_PEER));
}

// The fused total must close after deferred wake completion. The caller
// outranks the server, forcing a scheduling decision. Use switch counts
// to check ordering; host cycle deltas are nondeterministic.
TEST_F(FusedWake, the_composite_closes_past_the_deferred_seat)
{
    Fused f{};
    ASSERT_NO_FATAL_FAILURE(stage(&f, PRIO_ABOVE_SERVER, ON_ME, PRIO_LOW, ON_PEER));

    bench_seam_reset();
    uint32_t const before = g_switches;
    int32_t const rc = serve(&f);
    uint32_t const rows = g_bench_seam_reply_recv_rows;
    uint32_t const at_row = g_bench_seam_reply_recv_switches;
    bench_seam_reset();

    ASSERT_EQ(rc, static_cast<int32_t>(sizeof(f.sender_buf)));
    ASSERT_EQ(sched::current(), f.answered) << "the deferred seat never reached the scheduler";
    EXPECT_EQ(rows, 1u) << "the composite was not closed exactly once";
    EXPECT_GT(at_row, before) << "the composite closed before the deferred seat was discharged";
}

// A reply failure must replace the accepted mask with zero consumed bits.
TEST_F(FusedWake, a_reply_refusal_reports_no_notification_consumed)
{
    Line l{};
    ASSERT_NO_FATAL_FAILURE(seat_server_with_a_line(&l));

    kos_reply_recv_opts opts{};
    kos_reply_recv_opts_init(&opts, KOS_CAP_NONE, 0, KOS_TIMEOUT_NONE);
    opts.notify = l.bit;
    // Use an invalid reply capability to fail before opening any IRQ line.
    EXPECT_EQ(endpoint_reply_recv(0x7fffffffu, 0, 0, reinterpret_cast<uintptr_t>(&opts)),
              -KOS_EBADF);
    EXPECT_EQ(opts.notify, 0u) << "the accepted mask was reported back as consumed";

    // Control: the same error from the receive path must also write back zero.
    opts.notify = l.bit;
    EXPECT_EQ(endpoint_reply_recv(KOS_CAP_NONE, 0, 0, reinterpret_cast<uintptr_t>(&opts)),
              -KOS_EBADF);
    EXPECT_EQ(opts.notify, 0u);
}

// Unknown flags must still write back zero after opts validation succeeds.
TEST_F(FusedWake, an_undefined_flag_bit_reports_no_notification_consumed)
{
    Line l{};
    ASSERT_NO_FATAL_FAILURE(seat_server_with_a_line(&l));

    kos_reply_recv_opts opts{};
    kos_reply_recv_opts_init(&opts, KOS_CAP_NONE, FLAGS_WITH_AN_UNDEFINED_BIT, KOS_TIMEOUT_NONE);
    opts.notify = l.bit;
    EXPECT_EQ(endpoint_reply_recv(KOS_CAP_NONE, 0, 0, reinterpret_cast<uintptr_t>(&opts)),
              -KOS_EINVAL);
    EXPECT_EQ(opts.notify, 0u) << "the accepted mask was reported back as consumed";
}

// Reject buf while accepting opts to isolate payload validation.
TEST_F(FusedWake, an_unusable_payload_buffer_reports_no_notification_consumed)
{
    Line l{};
    ASSERT_NO_FATAL_FAILURE(seat_server_with_a_line(&l));

    uint8_t payload[4] = {};
    kos_reply_recv_opts opts{};
    kos_reply_recv_opts_init(&opts, KOS_CAP_NONE, 0, KOS_TIMEOUT_NONE);
    opts.notify = l.bit;
    g_ipc_seam_refuse_rw = reinterpret_cast<uintptr_t>(payload);
    int32_t const rc = endpoint_reply_recv(KOS_CAP_NONE, reinterpret_cast<uintptr_t>(payload),
                                           kos_call_lens_pack(0, sizeof(payload)),
                                           reinterpret_cast<uintptr_t>(&opts));
    g_ipc_seam_refuse_rw = 0;
    EXPECT_EQ(rc, -KOS_EFAULT);
    EXPECT_EQ(opts.notify, 0u) << "the accepted mask was reported back as consumed";
}

// A write-back failure must override the earlier error with EFAULT.
TEST_F(FusedWake, a_refused_write_back_of_the_consumed_mask_answers_efault)
{
    Line l{};
    ASSERT_NO_FATAL_FAILURE(seat_server_with_a_line(&l));

    kos_reply_recv_opts opts{};
    kos_reply_recv_opts_init(&opts, KOS_CAP_NONE, FLAGS_WITH_AN_UNDEFINED_BIT, KOS_TIMEOUT_NONE);
    opts.notify = l.bit;
    g_ipc_seam_refuse_write = reinterpret_cast<uintptr_t>(&opts.notify);
    int32_t const rc = endpoint_reply_recv(KOS_CAP_NONE, 0, 0,
                                           reinterpret_cast<uintptr_t>(&opts));
    g_ipc_seam_refuse_write = 0;
    EXPECT_EQ(rc, -KOS_EFAULT) << "the refused write-back was reported as the original error";
}

// A real event must return its bit, ruling out unconditional zero write-back.
TEST_F(FusedWake, a_consumed_notification_is_still_reported)
{
    Line l{};
    ASSERT_NO_FATAL_FAILURE(seat_server_with_a_line(&l));
    ASSERT_EQ(notify_signal(l.server, l.note), 0);
    ASSERT_EQ(notify_pending_of(l.server) & l.bit, l.bit);

    kos_reply_recv_opts opts{};
    kos_reply_recv_opts_init(&opts, KOS_CAP_NONE, 0, KOS_TIMEOUT_NONE);
    opts.notify = l.bit;
    EXPECT_EQ(endpoint_reply_recv(KOS_CAP_NONE, 0, 0, reinterpret_cast<uintptr_t>(&opts)),
              -KOS_ENOTIFY);
    EXPECT_EQ(opts.notify, l.bit);
}

// The fused wait over a line admits only a server pinned to that line's claim core, and a
// refused one consumes nothing: the pending bit is still there for the admitted retry.
TEST_F(FusedWake, a_fused_wait_over_a_line_refuses_a_server_not_pinned_to_its_claim_core)
{
    Line l{};
    ASSERT_NO_FATAL_FAILURE(seat_server_with_a_line(&l));
    ASSERT_EQ(notify_signal(l.server, l.note), 0);

    l.server->affinity = ON_ME | ON_PEER;
    kos_reply_recv_opts opts{};
    kos_reply_recv_opts_init(&opts, KOS_CAP_NONE, 0, KOS_TIMEOUT_NONE);
    opts.notify = l.bit;
    EXPECT_EQ(endpoint_reply_recv(KOS_CAP_NONE, 0, 0, reinterpret_cast<uintptr_t>(&opts)),
              -KOS_EPERM);
    EXPECT_EQ(opts.notify, 0u);
    EXPECT_EQ(notify_pending_of(l.server) & l.bit, l.bit) << "the refused wait consumed the bit";

    l.server->affinity = ON_ME;
    opts.notify = l.bit;
    EXPECT_EQ(endpoint_reply_recv(KOS_CAP_NONE, 0, 0, reinterpret_cast<uintptr_t>(&opts)),
              -KOS_ENOTIFY);
    EXPECT_EQ(opts.notify, l.bit);
}
