// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// WHAT A FUSED REPLY-RECEIVE OWES THE OTHER CORES, at two kernel cores, over the real
// endpoint_reply_recv and the real scheduler.
//
// One trap can ready several threads: the reply half readies the caller it answers, and the
// receive half readies whatever it pops or bounces. Neither may switch away before the receive
// half parks, so the LOCAL SEAT is deferred to the end of the body and only one thread can
// have it. THE PEER ANNOUNCEMENT IS A DIFFERENT OBLIGATION AND IS OWED TO EVERY ONE OF THEM:
// it names a priority and a placement, so a thread left out of it is left out entirely, and a
// thread this core may not run at all is reachable by nothing else. Deferring both together
// reaches the peers for the top thread and for nobody behind it.
//
// THE TWO ORDERS ARE SEPARATE ARMS. Which of the two readied threads the local seat keeps is
// decided by their priorities, so the thread that needs the announcement is sometimes the
// second one offered and sometimes the first, and a body that announced only one of those
// positions would pass half of this gate.
//
// THE ASK IS A COUNTER BEFORE IT IS A DOORBELL (kernel/sync/klock.cc), and the counter is what
// a host reads: kickos_kernel_core_resched_owed(), sampled from the seat of the core it is
// owed to. tests/unit/migrateask holds the ask's own ordering; this gate holds the SET of
// threads it is sent for.
//
// AND THE SAME BODY'S ERROR EXITS REPORT A CONSUMED NOTIFICATION MASK. `notify` is IN-OUT: in
// it is the mask the caller ACCEPTS, out it is the mask this call CONSUMED. An exit that
// leaves the field alone therefore reports every accepted line as consumed, and the caller's
// next act is to service lines that never fired.
//
// THE ARMS BELOW WALK EVERY EXIT PAST THE OPTIONS SNAPSHOT and not one of them: the reply
// refusal, an undefined flags bit, an unusable payload buffer, and the write-back's own
// refusal. What holds those exits together is a shape rather than a rule each one follows, so
// an arm here is also a gate on the shape, and a body that reaches this field from a plain
// `return` fails one of them whichever exit it added.

#include <string.h>

#include <kickos/arch/arch.h>
#include <kickos/cap.h>
#include <kickos/endpoint.h>
#include <kickos/instance.h>
#include <kickos/irq.h>
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
    constexpr uint32_t CORE_ME = 0;   // the core the server runs on and the fixture speaks as
    constexpr uint32_t CORE_PEER = 1; // the core an ask is read against

    constexpr uint32_t ON_ME = 1u << CORE_ME;
    constexpr uint32_t ON_PEER = 1u << CORE_PEER;

    constexpr uint8_t PRIO_SERVER = 10;
    constexpr uint8_t PRIO_ABOVE_SERVER = 11; // takes the seat off the server when it is picked
    constexpr uint8_t PRIO_HIGH = 9;
    constexpr uint8_t PRIO_LOW = 8;
    constexpr uint8_t PRIO_PEER_RUNNING = 1; // low enough that any ask above it is sent

    class FusedWake : public KSeam
    {
    };

    // Whether core `core` has been asked to reschedule, read from its own seat: the cell is
    // per core and every accessor resolves it through the fixture's core identity.
    bool owed_on(uint32_t core)
    {
        uint32_t const was = g_core;
        g_core = core;
        bool const owed = kickos_kernel_core_resched_owed() != 0;
        g_core = was;
        return owed;
    }

    // Clear the peer's cell so an arm reads only what it provoked: seating the threads asks
    // for each of them.
    void clear_peer_ask()
    {
        uint32_t const was = g_core;
        g_core = CORE_PEER;
        (void)kickos_kernel_core_resched_take();
        g_core = was;
    }

    // A server mid-transaction with both halves of a fused call ready to be served: a caller
    // parked on its reply capability, and a plain send queued behind it on the endpoint.
    //
    // The two threads' priorities and affinities are the arm's own, which is what lets one
    // geometry express both offer orders.
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
        // PARKED BEFORE THE FIRST PICK, because an arm may give one of them a priority above
        // the server's: left READY it would take the seat here and the arm would stage a
        // server that is not running.
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
        // The peer is RUNNING its own thread, which is what makes an ask meaningful: a core
        // with nothing seated is not asked at all.
        kernel().policy->on_remove(f->peer_running);
        f->peer_running->state = ThreadState::RUNNING;
        kernel().current[CORE_PEER] = f->peer_running;

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
            // A PLAIN send, not a call: it takes no reply capability, so the receive half
            // answers it and RETURNS instead of parking, and the guard is completed by its
            // destructor rather than handed to a park.
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

    // A seated server holding ONE bound notification line, nothing pending on it.
    struct Line
    {
        Thread* server;
        uint32_t irq;
        uint32_t bit;
    };

    // The arms below write `bit` into the accepted mask, and it is a REAL line on purpose: a
    // made-up mask names nothing this body could rearm or consume, so an arm built on one
    // could pass over a body that never reached its write-back at all.
    void seat_server_with_a_line(Line* l)
    {
        l->server = seat_pool(0, PRIO_SERVER);
        attach_caps(l->server, KICKOS_CAP_CHILD_WIDTH);
        {
            IrqLock lock;
            sched::reschedule();
        }
        l->irq = KCAP_INVALID;
        l->bit = 0;
        ASSERT_EQ(irq_claim(l->server, 5, 0, &l->irq), 0);
        ASSERT_EQ(irq_notify_bind(l->server, l->irq, &l->bit), 0);
        ASSERT_NE(l->bit, 0u);
        ASSERT_EQ(l->server->notify_pending, 0u);
    }

    // A flags word carrying one DEFINED bit beside one that is not, which is the shape a
    // caller built against a later ABI produces. A word of pure rubbish would also be refused
    // by a body that tested flags for equality with zero.
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

// THE ARM THE DEFECT FAILED. The reply half readies the higher thread, so the local seat keeps
// it and the sender behind it is the one an announcement has to name. It runs only on the
// peer, so the ask is the whole of its reachability.
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

// THE OTHER OFFER ORDER. The sender outranks the caller, so it is the one that displaces the
// other from the seat, and the announcement is owed to the thread offered FIRST.
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

// BOTH OBLIGATIONS IN ONE ARM, and the non-vacuity of the deferral: the seat still goes to the
// highest of the set, which here outranks the server and therefore takes the core, WHILE the
// peer is told about the other one. A body that announced everything and deferred nothing
// would leave the server seated.
TEST_F(FusedWake, the_seat_goes_to_the_highest_while_the_peer_hears_about_the_rest)
{
    Fused f{};
    ASSERT_NO_FATAL_FAILURE(stage(&f, PRIO_ABOVE_SERVER, ON_ME, PRIO_LOW, ON_PEER));

    ASSERT_EQ(serve(&f), static_cast<int32_t>(sizeof(f.sender_buf)));
    EXPECT_EQ(sched::current(), f.answered) << "the deferred seat was not fed the highest";
    EXPECT_TRUE(owed_on(CORE_PEER));
}

// AND THE COMPOSITE THAT PRICES ALL OF IT CLOSES PAST THE SEAT. The geometry is the one
// above: the answered caller outranks the server, so the deferred seat is really held and its
// destructor really runs the ready-queue selection, the peer announcement and the switch
// request. Those are the fused operation's own work, and a PH_REPLY_RECV_TOTAL closed while
// the guard is still alive leaves every one of them outside the row, which reads as an
// improvement over a release that bracketed them.
//
// THE SWITCH COUNT IS THE CLOCK, not the delta the bracket passes: bench_cyccnt is rdtsc
// here, so the only thing about a sample that is deterministic is WHEN it was taken.
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

// A reply refusal ends the call before any line is opened, so the consumed mask is zero and
// must be SAID to be zero: the accepted mask the caller wrote in is not an answer.
TEST_F(FusedWake, a_reply_refusal_reports_no_notification_consumed)
{
    Line l{};
    ASSERT_NO_FATAL_FAILURE(seat_server_with_a_line(&l));

    kos_reply_recv_opts opts{};
    kos_reply_recv_opts_init(&opts, KOS_CAP_NONE, 0, KOS_TIMEOUT_NONE);
    opts.notify = l.bit;
    // A reply capability that resolves to nothing: an ordinary recoverable client fault, not a
    // malformed argument, and the exit that used to return before the write-back.
    EXPECT_EQ(endpoint_reply_recv(0x7fffffffu, 0, 0, reinterpret_cast<uintptr_t>(&opts)),
              -KOS_EBADF);
    EXPECT_EQ(opts.notify, 0u) << "the accepted mask was reported back as consumed";

    // CONTROL: the same errno reported by a path that always reached the write-back, so the
    // arm above is not passing on a field nothing in this body ever writes.
    opts.notify = l.bit;
    EXPECT_EQ(endpoint_reply_recv(KOS_CAP_NONE, 0, 0, reinterpret_cast<uintptr_t>(&opts)),
              -KOS_EBADF);
    EXPECT_EQ(opts.notify, 0u);
}

// THE ARGUMENT REFUSALS PAST THE SNAPSHOT. An undefined flags bit is refused once the options
// struct has been proved writable, so the consumed mask is owed here exactly as it is on the
// reply refusal above.
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

// THE OTHER ONE: the single buffer the reply goes out of and the request comes back into. The
// seam refuses this arm's own buffer by address, `opts` itself still passing, so what is
// exercised is the buffer clause and not the struct clause above it.
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

// AND THE ONE THING THAT OUTRANKS THE ORIGINAL ERROR. The write-back can be refused in its own
// right, and a caller handed back the -KOS_EINVAL alone would read a field nothing wrote.
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

// AND A LINE THAT REALLY FIRED IS STILL REPORTED, which is what keeps every arm above from
// being satisfied by a body that writes zero unconditionally.
TEST_F(FusedWake, a_consumed_notification_is_still_reported)
{
    Line l{};
    ASSERT_NO_FATAL_FAILURE(seat_server_with_a_line(&l));
    ASSERT_EQ(irq_notify(l.server, l.irq), 0);
    ASSERT_EQ(l.server->notify_pending & l.bit, l.bit);

    kos_reply_recv_opts opts{};
    kos_reply_recv_opts_init(&opts, KOS_CAP_NONE, 0, KOS_TIMEOUT_NONE);
    opts.notify = l.bit;
    EXPECT_EQ(endpoint_reply_recv(KOS_CAP_NONE, 0, 0, reinterpret_cast<uintptr_t>(&opts)),
              -KOS_ENOTIFY);
    EXPECT_EQ(opts.notify, l.bit);
}
