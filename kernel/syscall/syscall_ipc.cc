// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// send, recv and call MUST be entered with no caller-held IrqLock: each releases its own
// before the resume barrier, and a spanning caller lock keeps BASEPRI raised across
// wq_confirm_resume and livelocks ARM.

#include <kickos/ampwindow.h>
#include <kickos/bench.h>
#include <kickos/cap.h>
#include <kickos/diag.h>
#include <kickos/endpoint.h>
#include <kickos/instance.h>
#include <kickos/irq.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/thread.h>
#include <kickos/time.h>

#include <kickos/sys/abi.h>
#include <kickos/sys/errno.h>

#include "syscall_internal.h"

namespace kickos
{
    namespace
    {
        // Distinct from ipc.badge_out == 0, which means the receiver asked for no info.
        constexpr uint32_t KOS_BADGE_NONE = 0;

        // THE ONE COMPLETION SITE for a wake a fused body readied and deliberately did not
        // reschedule. Both halves of a reply-receive ready threads, and neither may switch
        // away before the receive half parks, so the LOCAL SEAT is owed once at the end of
        // the whole body.
        //
        // ONLY the local seat. The peer announcement is a separate obligation owed to EVERY
        // readied thread at its own priority, and offer discharges it as each arrives: one
        // seat can hold one thread, so a guard that deferred the announcement too would reach
        // the peers for the top thread alone, and a thread the local pick may not place at
        // all would be announced nowhere.
        //
        // The invariant that makes the seat total: completion happens in the destructor, which
        // runs on every return out of the scope holding the guard, and the one path that may
        // leave it empty is the park, which hands the thread to wq_block's own reschedule
        // rather than dropping it. `take` is what makes those two exclusive, so no return, no
        // error return and no park can forget one.
        //
        // A NORETURN EXIT UNDER A LIVE GUARD WOULD DEFEAT THAT, sched::exit_current unwinding
        // nothing. Both guards are constructed past their body's park_cancel_pending check,
        // which is the only such exit in reach, and a new one must stay outside their scope.
        class DeferredWake
        {
        public:
            DeferredWake() = default;
            DeferredWake(DeferredWake const&) = delete;
            DeferredWake& operator=(DeferredWake const&) = delete;

            ~DeferredWake()
            {
                Thread* const t = take();
                if (t != nullptr)
                {
                    sched::resched_after_wake(t);
                }
            }

            // Announces every thread offered and keeps the HIGHEST for the local seat: a
            // pick fed the lower of two would decline the higher and seat the wrong one, so
            // exactly one is deferred, and the one this call displaces from that place is
            // announced now. The survivor's own announcement rides its reschedule.
            void offer(Thread* t)
            {
                Thread* keep = t;
                Thread* announce = top_;
                if (top_ != nullptr and t->prio <= top_->prio)
                {
                    keep = top_;
                    announce = t;
                }
                top_ = keep;
#if KICKOS_KERNEL_CORES > 1
                if (announce != nullptr)
                {
                    sched::announce_ready(announce);
                }
#else
                (void)announce;
#endif
            }

            [[nodiscard]] Thread* take()
            {
                Thread* const t = top_;
                top_ = nullptr;
                return t;
            }

        private:
            Thread* top_ = nullptr;
        };

        void park_deadline_arm(Thread* t, uint32_t timeout_us)
        {
            if (timeout_us != KOS_TIMEOUT_NONE)
            {
                ktime_deadline_arm(t, timeout_us);
            }
        }

        // Claim an endpoint slot and hand it back with EVERY field of Endpoint at the value
        // the type declares. A pooled slot keeps its last occupant's fields, so one this
        // caller does not overwrite still names that occupant. -1 where the pool is full,
        // and `*out` untouched there.
        [[nodiscard]] int endpoint_slot_claim(Endpoint** out)
        {
            int const i = kernel().endpoints.alloc();
            Endpoint* const ep = kernel().endpoints.at(i); // total over alloc()'s -1
            if (ep == nullptr)
            {
                return -1;
            }
            *ep = Endpoint{};
            *out = ep;
            return i;
        }

#if KICKOS_AMP_NODE
        // Publish `len` bytes of `c`'s buffer at `buf` to the far endpoint `e`, and answer the
        // byte count or the errno the window's refusal maps to. `tag` is the route a reply
        // takes back, or REPLY_TAG_NONE where the sender does not park.
        //
        // A KERNEL STAGE, because amp::send copies from a kernel pointer and `buf` names user
        // memory in a space the window layer may not reach into. Both copies run under the
        // caller's IrqLock and the stage is KOS_EP_MSG_MAX of syscall stack.
        int32_t far_publish(Thread* c, Endpoint const* e, uintptr_t buf, size_t len,
                            amp::ReplyTag const& tag)
        {
            uint8_t stage[KOS_EP_MSG_MAX];
            if (not kaccess_from_user(stage, user_space_of(c), buf, len))
            {
                return -KOS_EFAULT;
            }
            amp::Sent const rc = amp::send(endpoint_far_node(e), e->far_port, tag, stage,
                                           static_cast<uint32_t>(len));
            if (rc == amp::Sent::OK)
            {
                return static_cast<int32_t>(len);
            }
            if (rc == amp::Sent::FULL)
            {
                return -KOS_EBUSY;
            }
            if (rc == amp::Sent::DEPTH)
            {
                return -KOS_EPIPE;
            }
            return -KOS_EINVAL;
        }
#endif
    }

    // Two counters track an endpoint, endpoint_refs (all caps) and recv_holders (WAIT-bearing
    // caps); a rollback must unwind BOTH.
    int endpoint_create(uint32_t* out_cap)
    {
        IrqLock lock;
        *out_cap = KCAP_INVALID;
        Thread* c = sched::current();
        if (c == nullptr)
        {
            return -KOS_EPERM;
        }
        // Before the slot, as at the other three creators. The endpoint pool is the fleet's
        // smallest at four slots, so this is the ceiling that binds first.
        if (not task_object_admit(CapType::CAP_ENDPOINT, c->task))
        {
            return -KOS_EOVERFLOW; // this task holds its ceiling of endpoints already
        }
        Endpoint* ep = nullptr;
        int const i = endpoint_slot_claim(&ep);
        if (i < 0)
        {
            return -KOS_ENOMEM;
        }
        ep->recv_holders = 1; // creator holds a WAIT-bearing cap
        kernel().endpoint_refs[i] = 1;
        int const obj = kernel().endpoints.handle_for(i);
        int const rc = cap_install(c, obj, CapType::CAP_ENDPOINT,
                                   CAP_WAIT | CAP_SIGNAL | CAP_TRANSFER, out_cap);
        if (rc != 0)
        {
            kernel().endpoint_refs[i] = 0;
            kernel().endpoints.free(obj);
            return rc;
        }
        return 0;
    }

#if KICKOS_AMP_NODE
    static_assert(amp::NODE_MAX + 1u <= 0xFFu,
                  "a node index plus one must fit Endpoint::far_node, whose 0 is the local "
                  "sentinel");
    static_assert(amp::PORT_MAX <= 0xFFu, "a port must fit Endpoint::far_port");
    // The far arm validates the WHOLE caller sequence, so no count of retries can bring a
    // sequence round again while the caller still holds it. Widen call_seq and the wire width
    // must follow it here.
    static_assert(amp::REPLY_SEQ_MASK == (1u << (8u * sizeof(Thread::call_seq))) - 1u,
                  "the reply tag's validated width must be the whole of Thread::call_seq");
#endif

#if KICKOS_AMP_NODE
    // Caller holds IrqLock and owes the privilege gate.
    int amp_endpoint_mint(Thread* c, uint32_t node, uint32_t port, uint32_t* out_cap)
    {
        *out_cap = KCAP_INVALID;
        // port_minted is total over BOTH arguments, so it is the whole range check too.
        // PORT_REPLY names a CLASS and never a service: a call published on it lands in the
        // peer's reply ring, where a take answers CLASS and drops it.
        if (not amp::port_minted(node, port) or node == amp::self() or port == amp::PORT_REPLY)
        {
            return -KOS_EINVAL;
        }
        Endpoint* ep = nullptr;
        int const i = endpoint_slot_claim(&ep);
        if (i < 0)
        {
            return -KOS_ENOMEM;
        }
        ep->far_node = static_cast<uint8_t>(node + 1u);
        ep->far_port = static_cast<uint8_t>(port);
        kernel().endpoint_refs[i] = 1;
        int const obj = kernel().endpoints.handle_for(i);
        // CAP_SIGNAL ALONE, AND NOT AN OVERSIGHT TO BE WIDENED: a receive resolves for
        // CAP_WAIT, so this refuses a far endpoint there with no branch of its own, and
        // endpoint_server_set is unreachable on one as a consequence.
        int const rc = cap_install(c, obj, CapType::CAP_ENDPOINT, CAP_SIGNAL, out_cap);
        if (rc != 0)
        {
            kernel().endpoint_refs[i] = 0;
            kernel().endpoints.free(obj);
            return rc;
        }
        return 0;
    }
#endif

    // Mints an endpoint whose receiver runs in ANOTHER node's kernel. `node` and `port` name
    // a crossing the static mint already allows; no capability authorises the crossing
    // itself (docs/design-multicore.md N8), so this is privileged and takes no authority bit.
    int amp_endpoint_create(uint32_t node, uint32_t port, uint32_t* out_cap)
    {
        *out_cap = KCAP_INVALID;
#if KICKOS_AMP_NODE
        IrqLock lock;
        Thread* c = sched::current();
        if (c == nullptr or not c->privileged)
        {
            return -KOS_EPERM;
        }
        return amp_endpoint_mint(c, node, port, out_cap);
#else
        (void)node;
        (void)port;
        return -KOS_ENOSYS;
#endif
    }

    // Returns bytes transferred (>= 0), or -KOS_E* (EINVAL oversize, EFAULT bad buffer,
    // EBADF/EPERM bad cap or missing SIGNAL, EPIPE dead endpoint or last receiver left,
    // ETIMEDOUT `timeout_us` elapsed, ECANCELED cancelled while parked; in the last three
    // cases nothing was sent). `timeout_us` bounds the PARK only.
    //
    // A FAR ENDPOINT NEVER PARKS, so it never times out and answers -KOS_EBUSY where the
    // peer's ring is full (docs/reference/ipc-call-reply.md).
    int32_t endpoint_send(uint32_t cap, uintptr_t buf, size_t len, uint32_t timeout_us)
    {
        if (len > KOS_EP_MSG_MAX)
        {
            return -KOS_EINVAL; // oversize is rejected, never clamped
        }
        if (not user_readable_ok(buf, len))
        {
            return -KOS_EFAULT; // checked once, in caller context
        }
        Thread* c = sched::current();
        if (c == nullptr)
        {
            return -KOS_EPERM;
        }
        uint32_t epoch = 0;
        {
            IrqLock lock;
            if (park_cancel_pending(c))
            {
                sched::exit_current(KOS_EXIT_CANCELLED, sched::EXIT_RETURN); // noreturn
            }
            int err = 0;
            Endpoint* e = static_cast<Endpoint*>(
                cap_resolve_e(c, cap, CapType::CAP_ENDPOINT, CAP_SIGNAL, &err));
            if (e == nullptr)
            {
                return -err; // EBADF (bad cap) or EPERM (no SIGNAL right)
            }
#if KICKOS_AMP_NODE
            // BEFORE the dead-endpoint test and not after: a far endpoint carries no local
            // receiver, so recv_holders is 0 on one by construction and the test below would
            // answer -KOS_EPIPE for every far send.
            if (endpoint_is_far(e))
            {
                return far_publish(c, e, buf, len, amp::REPLY_TAG_NONE);
            }
#endif
            if (e->recv_holders == 0)
            {
                return -KOS_EPIPE; // dead endpoint: no receiver can ever exist
            }
            Thread* w = wq_pop_highest(e->recv_waiters);
            if (w != nullptr)
            {
                size_t n = len;
                if (w->ipc.len < n)
                {
                    n = w->ipc.len; // datagram truncation, not an error
                }
                if (not ep_copy(ipc_buf_space(w), w->ipc.buf, user_space_of(c), buf, n))
                {
                    // The receiver is answered and not dropped, being already off
                    // recv_waiters. EFAULT and not a zero-length arrival: a prefix may have
                    // landed in its buffer, so 0 would misdescribe those bytes.
                    w->wait_result = -KOS_EFAULT;
                    sched::wake(w);
                    return -KOS_EFAULT;
                }
                (void)write_recv_info(user_space_of(w), w->ipc.badge_out, KOS_BADGE_NONE,
                                      KCAP_INVALID);
                w->wait_result = static_cast<intptr_t>(n);
                sched::wake(w);
                return static_cast<int>(n); // did not block: no resume barrier
            }
            c->ipc.buf = buf;
            c->ipc.len = len;
            c->ipc.badge_out = 0;
            c->call_state = CALL_NONE; // B1: a plain sender is never a call
            epoch = c->switch_count;
            park_deadline_arm(c, timeout_us);
            wq_block(e->send_waiters, WAIT_EP_SEND, e);
        }
        wq_confirm_resume(c, epoch);
        // n (>= 0), -KOS_EPIPE (last receiver left), -KOS_ETIMEDOUT (ktime_on_timer), or
        // -KOS_ECANCELED (this parked caller was cancelled)
        return static_cast<int32_t>(c->wait_result);
    }

    // It never parks: a fault record must reach neither a driver mid-write nor one already
    // gone. Resolved through the kernel's own identity ref on the published endpoint
    // (cap_console_publish), and `buf` is kernel memory, so there is no user_readable_ok.
    int32_t cap_console_deliver(char const* buf, size_t len)
    {
        IrqLock lock;
        int target = 0;
        if (not cap_console_target(&target))
        {
            return 0;
        }
        Endpoint* e = kernel().endpoints.resolve(target);
        if (e == nullptr or e->recv_holders == 0)
        {
            return 0;
        }
        Thread* w = wq_pop_highest(e->recv_waiters);
        if (w == nullptr)
        {
            return 0;
        }
        size_t n = len;
        if (w->ipc.len < n)
        {
            n = w->ipc.len; // datagram truncation, as for any sender
        }
        if (not kaccess_to_user(ipc_buf_space(w), w->ipc.buf, buf, n))
        {
            n = 0; // the receiver keeps a partial line and is told nothing arrived
        }
        // Refused the same way and never by an early return: the waiter is already off
        // recv_waiters, so returning here would leave it parked with nothing left to wake it.
        if (not write_recv_info(user_space_of(w), w->ipc.badge_out, KOS_BADGE_NONE, KCAP_INVALID))
        {
            n = 0;
        }
        w->wait_result = static_cast<intptr_t>(n);
        sched::wake(w);
        return static_cast<int32_t>(n);
    }

    // The RECEIVE half under an ALREADY-HELD IrqLock, shared by the plain receive, its timed
    // form and the fused reply-receive. `badge_out` is the resolved out-pointer, 0 for the
    // info-less posture, and `cap_len` is already clamped. `dw` is the caller's guard, which
    // may already carry a thread the reply half woke; this body adds its own and either hands
    // the top one to the park or leaves it for the guard to complete.
    // Answers the byte count or -KOS_E*; on a park it sets *parked and seats *epoch, and the
    // caller owes wq_confirm_resume and reads wait_result.
    int32_t endpoint_recv_locked(Thread* c, uint32_t cap, uintptr_t buf, size_t cap_len,
                                 uintptr_t badge_out, uint32_t timeout_us, DeferredWake* dw,
                                 bool* parked, uint32_t* epoch)
    {
        KICKOS_BENCH_MARK(bm_rlocked);
        *parked = false;
        KICKOS_BENCH_MARK(bm_rresolve);
        int err = 0;
        Endpoint* e = static_cast<Endpoint*>(
            cap_resolve_e(c, cap, CapType::CAP_ENDPOINT, CAP_WAIT, &err));
        if (e == nullptr)
        {
            return -err; // EBADF (bad cap) or EPERM (no WAIT right)
        }
        endpoint_server_set(e, c); // the conventional receiver (D2 boost target)
        // A reschedule inside the scan moves current() onto the woken caller, and the
        // wq_block below re-reads it, so it would park that caller. Every wake here
        // defers its reschedule to the guard.
        KICKOS_BENCH_SPAN(PH_RECV_RESOLVE, bm_rresolve);
        KICKOS_BENCH_MARK(bm_rscan);
        while (true)
        {
            Thread* s = wq_pop_highest(e->send_waiters);
            if (s == nullptr)
            {
                break;
            }
            if (s->call_state == CALL_SEND_WAIT)
            {
                // The reply cap would be minted into OUR table, which an info-less recv
                // cannot deliver: bounce this caller and keep scanning for plain traffic
                // behind it.
                if (badge_out == 0)
                {
                    s->call_state = CALL_NONE; // B1: clear before waking
                    s->wait_result = -KOS_ENOSYS;
                    // Revert the D2 boost this bounced caller pinned on us; it is
                    // off-queue with CALL_NONE, so the funnel now excludes it.
                    uint8_t const np = thread_effective_prio(c);
                    if (np != c->prio)
                    {
                        sched::set_prio(c, np);
                    }
                    if (sched::wake_no_resched(s))
                    {
                        dw->offer(s);
                    }
                    continue;
                }
                // B3: probe the mint before committing; a plain sender behind this one
                // can still be served.
                if (not cap_can_take_reply(c))
                {
                    s->call_state = CALL_NONE;
                    s->wait_result = -KOS_EMFILE; // OUR table, reported to the caller
                    uint8_t const np = thread_effective_prio(c);
                    if (np != c->prio)
                    {
                        sched::set_prio(c, np);
                    }
                    if (sched::wake_no_resched(s))
                    {
                        dw->offer(s);
                    }
                    continue;
                }
                size_t n = s->ipc.len;
                if (cap_len < n)
                {
                    n = cap_len; // truncate the request into our capacity
                }
                bool ok = ep_copy(user_space_of(c), buf, ipc_buf_space(s), s->ipc.buf, n);
                if (ok)
                {
                    uint32_t rcap = KCAP_INVALID;
                    // Not inside the assert: a compiled-out condition would drop the mint.
                    int const minted = cap_install_reply(c, s, &rcap);
                    KICKOS_ASSERT(minted == 0);
                    ok = write_recv_info(user_space_of(c), badge_out, KOS_BADGE_NONE, rcap);
                    if (not ok)
                    {
                        // An undisclosed mint is one nothing can ever spend, and this
                        // caller would park on a handle we were never told.
                        bool const undone = cap_uninstall_reply(c, rcap, s);
                        KICKOS_ASSERT(undone);
                        (void)undone;
                    }
                }
                if (not ok)
                {
                    // NOT a `continue` like the two bounces above: the refusal does not
                    // say WHICH end went away, and if it was OUR buffer then every
                    // remaining sender would be popped and faulted in turn. One popped
                    // caller is the narrower loss.
                    s->call_state = CALL_NONE; // B1: clear before waking
                    s->wait_result = -KOS_EFAULT;
                    uint8_t const np = thread_effective_prio(c);
                    if (np != c->prio)
                    {
                        sched::set_prio(c, np);
                    }
                    if (sched::wake_no_resched(s))
                    {
                        dw->offer(s);
                    }
                    return -KOS_EFAULT;
                }
                s->ipc.len = s->call_rx_cap;
                s->ipc.badge_out = 0;
                s->call_state = CALL_REPLY_WAIT;
                reply_donor_park(c, s);
                // D1: inherit the caller's priority for the transaction.
                if (s->prio > c->prio)
                {
                    sched::set_prio(c, s->prio);
                }
                return static_cast<int>(n); // process, then kos_reply the cap
            }
            size_t n = s->ipc.len;
            if (cap_len < n)
            {
                n = cap_len; // truncate into the receiver's capacity
            }
            if (not ep_copy(user_space_of(c), buf, ipc_buf_space(s), s->ipc.buf, n))
            {
                // Both ends are answered and the scan stops, for the reason the call arm
                // above gives: a refusal names no end, so continuing could fault the
                // whole queue on our own lost buffer.
                s->wait_result = -KOS_EFAULT;
                if (sched::wake_no_resched(s))
                {
                    dw->offer(s);
                }
                return -KOS_EFAULT;
            }
            (void)write_recv_info(user_space_of(c), badge_out, KOS_BADGE_NONE, KCAP_INVALID);
            s->wait_result = static_cast<intptr_t>(n);
            if (sched::wake_no_resched(s))
            {
                dw->offer(s);
            }
            return static_cast<int>(n);
        }
        KICKOS_BENCH_SPAN(PH_RECV_SCAN, bm_rscan);
        KICKOS_BENCH_MARK(bm_rpark);
        c->ipc.buf = buf;
        c->ipc.len = cap_len;
        c->ipc.badge_out = badge_out;
        *epoch = c->switch_count;
        park_deadline_arm(c, timeout_us);
        // THE PARK IS THE ONE EXIT THAT LEAVES THE GUARD EMPTY, and it owes the ask itself:
        // this thread goes BLOCKED, so the switch's RUNNING -> READY store never runs and
        // announces nothing, and a deferred wake dropped here would reach no peer at all.
        wq_block(e->recv_waiters, WAIT_EP_RECV, e, dw->take());
        KICKOS_BENCH_SPAN(PH_RECV_PARK, bm_rpark);
        *parked = true;
        KICKOS_BENCH_SPAN(PH_RECV_LOCKED, bm_rlocked);
        return 0;
    }

    // One in-place buffer: request out, reply back. Returns reply bytes (>= 0), or -KOS_E*
    // (EINVAL oversize, EFAULT bad buffer, EBADF/EPERM bad cap or no SIGNAL, EPIPE dead
    // endpoint or server died, EMFILE the SERVER's table is full or at its inbound reply
    // bound, ENOSYS server took an info-less recv, ETIMEDOUT `timeout_us` elapsed, ECANCELED
    // cancelled while parked on either half). `timeout_us` bounds the WHOLE call: ONE deadline
    // covers the wait on send_waiters AND the wait for the reply.
    //
    // A FAR CALL parks on one wait only, for the reply, and diverges on back-pressure alone:
    // -KOS_EBUSY where the peer's ring is full, with nothing mutated
    // (docs/reference/ipc-call-reply.md).
    int32_t endpoint_call(uint32_t cap, uintptr_t buf, size_t send_len, size_t recv_cap,
                          uint32_t timeout_us)
    {
        KICKOS_BENCH_MARK(bm_total);
        if (send_len > KOS_EP_MSG_MAX)
        {
            return -KOS_EINVAL; // oversize is rejected, like send
        }
        if (recv_cap > KOS_EP_MSG_MAX)
        {
            recv_cap = KOS_EP_MSG_MAX; // reply capacity clamp is harmless
        }
        KICKOS_BENCH_MARK(bm_validate);
        // The one buffer is read (request) then written (reply): validate both here,
        // in caller context, once.
        if (not user_readable_and_writable_ok(buf, send_len, recv_cap))
        {
            return -KOS_EFAULT;
        }
        KICKOS_BENCH_SPAN(PH_CALL_VALIDATE, bm_validate);
        Thread* c = sched::current();
        if (c == nullptr)
        {
            return -KOS_EPERM;
        }
        uint32_t epoch = 0;
        {
            IrqLock lock;
            // AHEAD OF THE TRANSACTION, not at either park below: the receiver-present arm
            // pops the receiver, bumps call_seq, mints the reply cap and writes the receiver's
            // info before it parks, and an exit taken there would strand a receiver that is
            // off its queue holding a cap for a caller that will never reply.
            if (park_cancel_pending(c))
            {
                sched::exit_current(KOS_EXIT_CANCELLED, sched::EXIT_RETURN); // noreturn
            }
            KICKOS_BENCH_MARK(bm_locked);
            // The two controls, taken under the same interrupt mask as the brackets they
            // calibrate. PH_NEST is PH_NULL inside an enclosing span, so it prices the
            // accumulator call PH_NULL cannot see.
            KICKOS_BENCH_MARK(bm_nest);
            KICKOS_BENCH_MARK(bm_null);
            KICKOS_BENCH_SPAN(PH_NULL, bm_null);
            KICKOS_BENCH_SPAN(PH_NEST, bm_nest);
            int err = 0;
            KICKOS_BENCH_MARK(bm_resolve);
            Endpoint* e = static_cast<Endpoint*>(
                cap_resolve_e(c, cap, CapType::CAP_ENDPOINT, CAP_SIGNAL, &err));
            KICKOS_BENCH_SPAN(PH_CALL_RESOLVE, bm_resolve);
            if (e == nullptr)
            {
                return -err; // EBADF (bad cap) or EPERM (no SIGNAL right)
            }
            // WHETHER THE RECEIVER IS IN ANOTHER KERNEL, decided immediately after the
            // resolve because the order is load-bearing: a far endpoint carries no local
            // receiver, so recv_holders is 0 on one by construction and a dead-endpoint
            // refusal taken first would answer -KOS_EPIPE for every far call.
            bool const far = endpoint_is_far(e);
            if (not far and e->recv_holders == 0)
            {
                return -KOS_EPIPE; // dead endpoint: no receiver can ever exist
            }
            KICKOS_BENCH_MARK(bm_peek);
            Thread* w = wq_peek_highest(e->recv_waiters);
            KICKOS_BENCH_SPAN(PH_CALL_PEEK, bm_peek);
#if KICKOS_AMP_NODE
            if (far)
            {
                int const idx = kernel().threads.index_of(c);
                KICKOS_ASSERT(idx >= 0); // idle is the one TCB outside the pool and calls nothing
                // The seq is DECIDED here and committed only once the publication is taken:
                // a refused call must leave the caller exactly as it found it.
                uint16_t const seq = static_cast<uint16_t>(c->call_seq + 1u);
                amp::ReplyTag const tag = {kernel().threads.handle_for(idx),
                                           amp::reply_seq(seq)};
                int32_t const sent = far_publish(c, e, buf, send_len, tag);
                if (sent < 0)
                {
                    return sent;
                }
                c->call_seq = seq;
                c->ipc.buf = buf;
                c->ipc.len = recv_cap;
                c->ipc.badge_out = 0;
                c->call_rx_cap = recv_cap;
                c->call_state = CALL_REPLY_WAIT;
                // No donation, where the local arm below donates at D1: there is no thread on
                // the far side to raise and no seam by which this node's priority reaches a
                // peer's scheduler (N6e).
                epoch = c->switch_count;
                park_queueless(c, WAIT_EP_FAR_REPLY, e);
                park_deadline_arm(c, timeout_us);
                // Nothing was woken, so no wake carries the switch: this park owes its own.
                sched::reschedule();
            }
            else
#endif
            if (w != nullptr)
            {
                // Fastpath: a receiver is parked. Probe BEFORE popping (B3): a popped
                // receiver we cannot serve would be stranded off-queue forever.
                if (w->ipc.badge_out == 0)
                {
                    return -KOS_ENOSYS; // M3: info-less receiver cannot host a call
                }
                if (w->dying)
                {
                    // A half-torn table must never take a new cap: the sweep may already
                    // have passed the slot, so the mint would outlive the thread and strand
                    // this caller. The sweep drops IrqLock between chunks.
                    return -KOS_EPIPE;
                }
                KICKOS_BENCH_MARK(bm_probe);
                if (not cap_can_take_reply(w))
                {
                    // The RECEIVER's table or its reply bound, not ours: no side effects yet.
                    return -KOS_EMFILE;
                }
                KICKOS_BENCH_SPAN(PH_CALL_PROBE, bm_probe);
                KICKOS_BENCH_MARK(bm_pop);
                (void)wq_pop_highest(e->recv_waiters); // == w: the queue cannot change here
                KICKOS_BENCH_SPAN(PH_CALL_POP, bm_pop);
                size_t n = send_len;
                if (w->ipc.len < n)
                {
                    n = w->ipc.len; // receiver-side request truncation
                }
                KICKOS_BENCH_MARK(bm_copy);
                if (not ep_copy(ipc_buf_space(w), w->ipc.buf, user_space_of(c), buf, n))
                {
                    // Ahead of the mint and of every mutation of `c`, so this caller has
                    // nothing to unwind; the receiver is answered because the pop already
                    // took it off recv_waiters.
                    w->wait_result = -KOS_EFAULT;
                    sched::wake(w);
                    return -KOS_EFAULT;
                }
                KICKOS_BENCH_SPAN(PH_CALL_COPY, bm_copy);
                c->call_seq++; // new epoch BEFORE packing (the reply cap rides this seq)
                uint32_t rcap = KCAP_INVALID;
                KICKOS_BENCH_MARK(bm_mint);
                KICKOS_BENCH_MARK(bm_mint_cap);
                // Not inside the assert: a compiled-out condition would drop the mint.
                int const minted = cap_install_reply(w, c, &rcap);
                KICKOS_BENCH_SPAN(PH_CALL_MINT_CAP, bm_mint_cap);
                KICKOS_ASSERT(minted == 0);
                KICKOS_BENCH_MARK(bm_mint_info);
                if (not write_recv_info(user_space_of(w), w->ipc.badge_out, KOS_BADGE_NONE, rcap))
                {
                    // An undisclosed mint is one nothing can ever spend, and this caller would
                    // park on a handle the receiver was never told. Both ends are answered as
                    // the ep_copy refusal above answers them: `c` has not parked yet, and the
                    // pop already took `w` off recv_waiters.
                    bool const undone = cap_uninstall_reply(w, rcap, c);
                    KICKOS_ASSERT(undone);
                    (void)undone;
                    w->wait_result = -KOS_EFAULT;
                    sched::wake(w);
                    return -KOS_EFAULT;
                }
                KICKOS_BENCH_SPAN(PH_CALL_MINT_INFO, bm_mint_info);
                KICKOS_BENCH_SPAN(PH_CALL_MINT, bm_mint);
                w->wait_result = static_cast<intptr_t>(n);
                // The request was fully copied above, so the reply may overwrite buf.
                c->ipc.buf = buf;
                c->ipc.len = recv_cap;
                c->ipc.badge_out = 0;
                c->call_rx_cap = recv_cap;
                c->call_state = CALL_REPLY_WAIT;
                // D1: donate to the server so the CPU goes straight to it.
                if (c->prio > w->prio)
                {
                    KICKOS_BENCH_MARK(bm_donate);
                    sched::set_prio(w, c->prio);
                    KICKOS_BENCH_SPAN(PH_CALL_DONATE, bm_donate);
                }
                epoch = c->switch_count;
                KICKOS_BENCH_MARK(bm_park);
                park_queueless(c, WAIT_EP_REPLY, w);
                reply_donor_park(w, c); // off the ready set: `link` is free to re-use
                // Armed straight onto the reply wait, the fastpath having skipped the
                // send-side park. Must precede the wake, which switches away.
                park_deadline_arm(c, timeout_us);
                KICKOS_BENCH_SPAN(PH_CALL_PARK, bm_park);
                KICKOS_BENCH_MARK(bm_wake);
                sched::wake(w);
                KICKOS_BENCH_SPAN(PH_CALL_WAKE, bm_wake);
                // Both close INSIDE the lock: the brace below is where it releases and a
                // pended switch fires, so a span reaching past it would time the whole round
                // trip. And fastpath-only: a span shared with the slowpath arm would report
                // that arm's shorter body as this one's minimum.
                KICKOS_BENCH_SPAN(PH_CALL_LOCKED, bm_locked);
                KICKOS_BENCH_SPAN(PH_CALL_TOTAL, bm_total);
            }
            else
            {
                // Slowpath: park on send_waiters as a call; recv completes the mint and the
                // transfer in server context later.
                c->ipc.buf = buf;
                c->ipc.len = send_len;
                c->ipc.badge_out = 0;
                c->call_rx_cap = recv_cap;
                c->call_seq++; // new epoch for this call (packed at pop time)
                c->call_state = CALL_SEND_WAIT;
                // D2: boost the conventional server NOW if this caller outranks it.
                if (e->server != nullptr and c->prio > e->server->prio)
                {
                    KICKOS_BENCH_MARK(bm_sdonate);
                    sched::set_prio(e->server, c->prio);
                    KICKOS_BENCH_SPAN(PH_CALL_SLOW_DONATE, bm_sdonate);
                }
                epoch = c->switch_count;
                KICKOS_BENCH_MARK(bm_spark);
                // Armed ONCE and never re-armed: it rides along untouched when a server
                // later pops this caller onto its reply_waiters, and then bounds the reply
                // wait.
                park_deadline_arm(c, timeout_us);
                // Same wait kind as a plain sender; call_state is what separates the two.
                wq_block(e->send_waiters, WAIT_EP_SEND, e);
                KICKOS_BENCH_SPAN(PH_CALL_SLOW_PARK, bm_spark);
                KICKOS_BENCH_SPAN(PH_CALL_SLOW_LOCKED, bm_locked);
                KICKOS_BENCH_SPAN(PH_CALL_SLOW_TOTAL, bm_total);
            }
        }
        KICKOS_BENCH_MARK(bm_resume);
        wq_confirm_resume(c, epoch);
        c->call_state = CALL_NONE; // B1: single-writer-clean on EVERY return path
        KICKOS_BENCH_SPAN(PH_CALL_RESUME, bm_resume);
        // reply bytes, or -KOS_EPIPE/-KOS_EMFILE/-KOS_ENOSYS/-KOS_ETIMEDOUT/-KOS_ECANCELED
        return static_cast<int32_t>(c->wait_result);
    }

    // One-shot: the cap is consumed on EVERY exit. Under an ALREADY-HELD IrqLock. Returns 0,
    // or -KOS_E* (EBADF bad or non-reply cap, EFAULT bad reply buffer, ESRCH the caller is
    // gone or aborted). Whatever it wakes it offers to `dw`, never rescheduling itself.
    //
    // `answered` is set where the LOCAL arm reached its wake, which is what the composite
    // spans bracket. An explicit flag and not the return code: the far arm answers 0 and
    // -KOS_EFAULT identically and brackets nothing.
    int endpoint_reply_locked(Thread* c, uint32_t reply_cap, uintptr_t buf, size_t len,
                              DeferredWake* dw, bool* answered)
    {
        KICKOS_BENCH_MARK(bm_locked);
        KICKOS_BENCH_MARK(bm_lookup);
        CapEntry* e = cap_lookup(c, reply_cap);
        if (e == nullptr or e->type != static_cast<uint8_t>(CapType::CAP_REPLY))
        {
            return -KOS_EBADF;
        }
#if KICKOS_AMP_NODE
        // A caller in ANOTHER kernel, named through the pool's reserved band. The service
        // holding this capability cannot tell it from a local one and calls the same reply.
        //
        // The wire carries no errno, only bytes and a length: a caller that must tell an empty
        // reply from a service that died carries its own marker in the payload.
        if (ThreadPool::far_reply_is(static_cast<uint32_t>(cap_reply_handle(*e))))
        {
            uint32_t const record =
                ThreadPool::far_reply_record(static_cast<uint32_t>(cap_reply_handle(*e)));
            // Consumed on EVERY exit, exactly as the local arm below consumes it.
            e->gen++;
            e->type = static_cast<uint8_t>(CapType::CAP_EMPTY);
            e->rights = 0;
            cap_run_free_release(c->caps, reply_cap & KCAP_INDEX_MASK, e, &c->cap_free_head);
            cap_reply_released(c);
            uint8_t stage[KOS_EP_MSG_MAX];
            if (not kaccess_from_user(stage, user_space_of(c), buf, len))
            {
                // The record is freed and its call slot released even here: a service whose
                // buffer went away may not hold a peer's slot for the life of the image.
                amp::inbound_reply(record, stage, 0u);
                return -KOS_EFAULT;
            }
            amp::inbound_reply(record, stage, static_cast<uint32_t>(len));
            return 0;
        }
#endif
        Thread* caller = cap_reply_caller(*e); // full stale-resolve BEFORE consume
        // cap_run_free_release writes the free-list links over `obj`, so it must follow the
        // resolve above.
        e->gen++;
        e->type = static_cast<uint8_t>(CapType::CAP_EMPTY);
        e->rights = 0;
        cap_run_free_release(c->caps, reply_cap & KCAP_INDEX_MASK, e, &c->cap_free_head);
        cap_reply_released(c);
        // Must precede both funnel recomputes below: a donor left linked past its cap is a
        // boost the replier can never shed. A false return means the caller is parked on
        // ANOTHER server, so this reply owns nothing of it and completes as if it were gone.
        if (caller != nullptr and not reply_donor_unpark(c, caller))
        {
            caller = nullptr;
        }
        KICKOS_BENCH_SPAN(PH_REPLY_LOOKUP, bm_lookup);
        if (caller == nullptr)
        {
            // A timed call whose deadline expired leaves CALL_NONE behind, so
            // cap_reply_caller answers nullptr for a cap this server still legitimately
            // holds. The donation is reverted anyway; the caller is already awake with
            // -KOS_ETIMEDOUT.
            sched::set_prio(c, thread_effective_prio(c));
            return -KOS_ESRCH; // caller aborted or reused; the cap is consumed regardless
        }
        size_t n = len;
        if (caller->call_rx_cap < n)
        {
            n = caller->call_rx_cap; // reply truncation into the caller's capacity
        }
        KICKOS_BENCH_MARK(bm_copy);
        bool const ok = ep_copy(ipc_buf_space(caller), caller->ipc.buf, user_space_of(c), buf, n);
        KICKOS_BENCH_SPAN(PH_REPLY_COPY, bm_copy);
        // Refused past the point of no return, the cap being consumed and the donor unparked,
        // so the answer is never an early return: the caller is off its queue and this is the
        // last thing that can wake it.
        caller->wait_result = static_cast<intptr_t>(n);
        if (not ok)
        {
            caller->wait_result = -KOS_EFAULT;
        }
        caller->call_state = CALL_NONE;
        KICKOS_BENCH_MARK(bm_funnel);
        // D3: revert our donation through the single funnel.
        sched::set_prio(c, thread_effective_prio(c));
        KICKOS_BENCH_SPAN(PH_REPLY_FUNNEL, bm_funnel);
        *answered = true;
        KICKOS_BENCH_MARK(bm_wake);
        // D4: the caller is >= us whenever it donated. The reschedule is the CALLER's to
        // issue, so a fused body can park first.
        if (sched::wake_no_resched(caller))
        {
            dw->offer(caller);
        }
        KICKOS_BENCH_SPAN(PH_REPLY_WAKE, bm_wake);
        KICKOS_BENCH_SPAN(PH_REPLY_LOCKED, bm_locked);
        if (not ok)
        {
            return -KOS_EFAULT;
        }
        return 0;
    }

    int endpoint_reply(uint32_t reply_cap, uintptr_t buf, size_t len)
    {
        KICKOS_BENCH_MARK(bm_total);
        if (len > KOS_EP_MSG_MAX)
        {
            len = KOS_EP_MSG_MAX; // the caller's capacity clamps it anyway
        }
        KICKOS_BENCH_MARK(bm_validate);
        if (not user_readable_ok(buf, len))
        {
            return -KOS_EFAULT;
        }
        KICKOS_BENCH_SPAN(PH_REPLY_VALIDATE, bm_validate);
        Thread* c = sched::current();
        if (c == nullptr)
        {
            return -KOS_EPERM;
        }
        IrqLock lock;
        bool answered = false;
        int rc;
        {
            // SCOPED TIGHTER THAN THE SPAN BELOW ON PURPOSE. Nothing here parks, so the
            // standalone reply has no reason to defer its seat past the body, and leaving it
            // to the function's own return would put ready-queue selection, the peer
            // announcement and the switch after PH_REPLY_TOTAL closed: the row would then
            // read as an improvement over a release that bracketed them.
            DeferredWake dw;
            rc = endpoint_reply_locked(c, reply_cap, buf, len, &dw, &answered);
        }
        if (answered)
        {
            // Closes before the return, where `lock` releases and a pended switch fires.
            KICKOS_BENCH_SPAN(PH_REPLY_TOTAL, bm_total);
        }
        return rc;
    }

    // KOS_SYS_REPLY_RECV: one trap, one IrqLock, one sched::current() and one buffer validate
    // where the kos_reply then kos_recv every server loop open-codes pays two of each. `lens`
    // packs reply_len and recv_cap nine bits each; `buf` is ONE buffer, the reply going out of
    // it and the next request coming back into it, which is safe for the reason kos_call's
    // in-place buffer is: the reply is fully copied before this body parks.
    //
    // THE WAKE IS DEFERRED and that is the whole point of the fusion. On arm64, rv64, x86_64,
    // the LX6 and the sim arch_switch swaps INLINE, so a reply that rescheduled here would
    // switch away before the receive half parked, the caller would find no receiver and the
    // next call would take the slowpath. Both halves offer what they ready to one DeferredWake
    // whose destructor is the single completion, and the park hands it the thread instead.
    //
    // THE REPLY HALF'S FAILURES ARE NOT SYMMETRIC, and what splits them is whether this call
    // can still REPORT them. -KOS_ESRCH says the transaction the reply named is already over,
    // which is nothing about this server's ability to receive and nothing a caller has to be
    // told, so the receive half still runs. -KOS_EBADF and -KOS_EFAULT must reach the server,
    // and a receive on top of one overwrites the code with its own, so they end the call.
    // -KOS_EFAULT here is the CALLER's buffer and not this server's: `buf` was proved readable
    // and writable above, so the only side of the reply copy that can still be gone is the far
    // one. A server that treats it as fatal to itself lets one client end a shared service,
    // which is why the loops in <kickos/sys/console_service.h> classify it and keep serving.
    int32_t endpoint_reply_recv(uint32_t reply_cap, uintptr_t buf, uintptr_t lens,
                                uintptr_t opts)
    {
        KICKOS_BENCH_MARK(bm_frtotal);
        size_t reply_len = kos_call_lens_send(lens);
        size_t recv_cap = kos_call_lens_recv(lens);
        // CLAMPED, not refused, because that is what both halves did on their own: a reply
        // longer than the bound was clamped by the parked caller's capacity anyway, and a
        // receive capacity above it was harmless. The pack saturates at 511 rather than
        // masking, so a wild argument arrives above the bound and clamps here rather than
        // wrapping to a silent zero.
        if (reply_len > KOS_EP_MSG_MAX)
        {
            reply_len = KOS_EP_MSG_MAX;
        }
        if (recv_cap > KOS_EP_MSG_MAX)
        {
            recv_cap = KOS_EP_MSG_MAX;
        }
        if (opts == 0 or (opts & (alignof(uint32_t) - 1)) != 0)
        {
            return -KOS_EINVAL; // load-bearing for the privileged accesses below
        }
        Thread* c = sched::current();
        if (c == nullptr)
        {
            return -KOS_EPERM;
        }
        // IN-OUT, so readable as well as writable.
        if (not user_readable_and_writable_ok(opts, sizeof(kos_reply_recv_opts),
                                              sizeof(kos_reply_recv_opts)))
        {
            return -KOS_EFAULT;
        }
        // Copied ONCE, before anything can park: the struct stays user-writable, so a re-read
        // after the park would see whatever the caller has since put there. Only the IN
        // PREFIX is read, which ENDS AT `info` and not at `notify`: the notification mask is
        // IN-OUT, so a read stopping short of it accepts no line and every notification
        // silently stops ending a wait.
        kos_reply_recv_opts in = {};
        if (not kaccess_from_user(&in, user_space_of(c), opts,
                                  offsetof(kos_reply_recv_opts, info)))
        {
            return -KOS_EFAULT;
        }
        // The notification mask is the whole opt-in, there being no flag beside it.
        uint32_t const admit = in.notify;
        uint32_t notify_bits = 0;
        // THE SNAPSHOT ABOVE IS THE LINE: past it the field is proved writable and the
        // write-back at the bottom is owed on EVERY exit, the argument refusals below
        // included. That is the whole reason the rest of the body is a callable: the
        // write-back is the statement after its single call, so a `return` added anywhere
        // inside it lands ON the obligation instead of stepping over it, which no body whose
        // exits are plain `return` statements can promise.
        //
        // NOT an RAII guard on the DeferredWake pattern, which is the shape this would
        // otherwise be simplified into: a destructor runs after the return value is fixed, so
        // it can discharge the write but can never answer the -KOS_EFAULT a REFUSED write
        // owes the caller.
        auto const served = [&]() -> int32_t
        {
            if ((in.flags & ~KOS_RECV_NO_INFO) != 0)
            {
                return -KOS_EINVAL;
            }
            // ONE validate over the one buffer, read for the reply and written for the arrival.
            if (not user_readable_and_writable_ok(buf, reply_len, recv_cap))
            {
                return -KOS_EFAULT;
            }
            uintptr_t badge_out = opts + offsetof(kos_reply_recv_opts, info);
            if ((in.flags & KOS_RECV_NO_INFO) != 0)
            {
                badge_out = 0; // the info-less posture: nothing is written and calls bounce
            }
            uint32_t epoch = 0;
            bool parked = false;
            int32_t rc = 0;
            // Seated inside the lock below and READ after it on the parked path, which is why
            // it lives out here.
            uint32_t opened = 0;
            bool reply_refused = false;
            {
                IrqLock lock;
                if (park_cancel_pending(c))
                {
                    sched::exit_current(KOS_EXIT_CANCELLED, sched::EXIT_RETURN); // noreturn
                }
                {
                    // SCOPED TIGHTER THAN THE LOCK, as the standalone kos_reply scopes its
                    // own seat: released here, the ready-queue selection and the peer
                    // announcement the wake owes run INSIDE the composite below instead of
                    // after it closed.
                    DeferredWake dw;
                    // Discarded: this body brackets its own total, and the reply half's composite
                    // spans belong to the standalone kos_reply.
                    bool answered = false;
                    if (reply_cap != KOS_CAP_NONE)
                    {
                        int const rrc =
                            endpoint_reply_locked(c, reply_cap, buf, reply_len, &dw, &answered);
                        if (rrc != 0 and rrc != -KOS_ESRCH)
                        {
                            rc = rrc;
                            reply_refused = true;
                        }
                    }
                    if (not reply_refused)
                    {
                        // Narrowed to the lines this thread really serves, and each of them rearmed
                        // and opened to end the wait below. Leaving it is what takes the bits AND
                        // flags each taken line for its next rearm, which nothing else on this path
                        // does.
                        opened = irq_notify_wait_enter(c, admit);
                        if ((c->notify_pending & opened) != 0u)
                        {
                            // Answered rather than parked behind: a message queued in the same
                            // breath is taken by the next pass of the loop, which is why the
                            // notification-only return is -KOS_ENOTIFY and not an error.
                            rc = -KOS_ENOTIFY;
                        }
                        else
                        {
                            rc = endpoint_recv_locked(c, in.ep, buf, recv_cap, badge_out,
                                                      in.timeout_us, &dw, &parked, &epoch);
                        }
                        if (not parked)
                        {
                            // An answer that never blocked takes its bits HERE, under the lock this
                            // body already holds: nothing switched away, so the word is current.
                            notify_bits = irq_notify_wait_leave(c, opened);
                        }
                    }
                }
                if (not reply_refused)
                {
                    // THE LAST POINT BEFORE THIS THREAD CAN LOSE THE CPU, which is why the
                    // composite ends here and not at the return: where arch_switch PENDS, the
                    // switch takes at the brace below, and a close past it would put the whole
                    // interval the server waited for its next client inside a row M8.12 reads
                    // as a cost. A refusal served no request and is bracketed by nothing.
                    KICKOS_BENCH_SPAN(PH_REPLY_RECV_TOTAL, bm_frtotal);
                }
            }
            if (parked)
            {
                KICKOS_BENCH_MARK(bm_frtail);
                wq_confirm_resume(c, epoch);
                rc = static_cast<int32_t>(c->wait_result);
                {
                    // AND A PARKED ONE TAKES ITS BITS HERE, past the barrier, which is the
                    // whole of the asymmetry above. Where arch_switch only PENDS the switch,
                    // this body runs on for some instructions past wq_block before the switch
                    // takes, so a take inside that lock scope reads the word as it stood BEFORE
                    // the raise that woke this thread and answers -KOS_ENOTIFY with no bit in
                    // it. Mutual exclusion is not what is missing: the barrier must run with
                    // interrupts UNMASKED or the pended switch cannot take at all, which is why
                    // it is outside the scope and why this take follows it.
                    IrqLock lock;
                    notify_bits = irq_notify_wait_leave(c, opened);
                }
                KICKOS_BENCH_SPAN(PH_REPLY_RECV_TAIL, bm_frtail);
            }
            return rc;
        };
        int32_t const rc = served();
        // REACHED BY EVERY EXIT PAST THE SNAPSHOT, the reply refusal and the two argument
        // refusals included: zero consumed bits is an answer and not an absence, and the only
        // exits that may skip it are the ones above that never proved the field writable.
        //
        // Skipped where the caller ACCEPTED no line, and that is sound because `notify` is
        // IN-OUT: such a caller wrote 0 in, so leaving the field alone leaves 0, which is the
        // true count of what this wait consumed. IT DIES WITH THE INPUT HALF: make `notify`
        // OUT-only again and this skip starts leaving a stale value behind.
        //
        // The refusal below survives for the same reason. Where nothing was accepted there
        // are no bits a lost write could drop; where something was, the caller's own buffer
        // went away under it and is told so even though a message may have arrived.
        if (admit != 0
            and not kaccess_to_user(user_space_of(c),
                                    opts + offsetof(kos_reply_recv_opts, notify), &notify_bits,
                                    sizeof(notify_bits)))
        {
            return -KOS_EFAULT;
        }
        return rc;
    }

#if KICKOS_AMP_NODE
#if defined(KICKOS_ENABLE_SELFTEST)
    namespace
    {
        bool g_far_blind = false;
    }

    void endpoint_far_blind_arm(void)
    {
        g_far_blind = true;
    }

    bool endpoint_far_blind_take(void)
    {
        bool const armed = g_far_blind;
        g_far_blind = false;
        return armed;
    }
#endif

    // Reached from the doorbell service body, so this core's interrupts are masked and that
    // mask is the whole of a one-core kernel's exclusion.
    bool endpoint_far_reply_deliver(uint32_t from, amp::ReplyTag const& tag, void const* payload,
                                    uint32_t len)
    {
        Thread* caller =
            cap_reply_thread(tag.thread, amp::reply_seq(tag.seq), amp::REPLY_SEQ_MASK);
        if (caller == nullptr)
        {
            return false;
        }
        // THE FIFTH CLAUSE, and the only cross-node one: the four above are satisfied by any
        // caller parked in a call, a LOCAL one included, so without this node 2 completes a
        // call node 0 made to node 1.
        Endpoint const* const e = caller->wait_far_endpoint();
        if (e == nullptr or endpoint_far_node(e) != from)
        {
            return false;
        }
        size_t n = len;
        if (caller->call_rx_cap < n)
        {
            n = caller->call_rx_cap; // reply truncation into the caller's capacity
        }
        intptr_t result = static_cast<intptr_t>(n);
        bool const copy_ok = kaccess_to_user(ipc_buf_space(caller), caller->ipc.buf, payload, n);
        // NOT ASSERTED: this copies into a parked thread from a masked handler. The code
        // travels on wait_result and not on the wire, THIS node writing its own TCB.
        if (not copy_ok)
        {
            result = -KOS_EFAULT;
            amp::count_deliver_fault();
        }
        caller->wait_result = result;
        caller->call_state = CALL_NONE;
        caller->clear_wait_edge();
        sched::wake(caller);
        return true;
    }

    // A call from another kernel, reaching a thread of this one. Runs from the doorbell service
    // body with this core's interrupts masked, and takes no lock for that reason.
    //
    // TRUE where a receiver took it and the call slot is now this node's record of the caller,
    // released when the reply is sent. FALSE where nothing took it, and the slot is released at
    // once so the ring does not fill behind a service that is not there.
    //
    // EVERY FALSE HERE IS ANSWERED BY dispatch_call, which publishes a zero-length reply
    // carrying the tag: a refusal past the pop leaves a far caller parked, and under
    // KOS_TIMEOUT_NONE nothing would ever wake it. So no arm below may grow an answer of its
    // own, and none may return without reaching this bool.
    bool endpoint_far_call_deliver(uint32_t from, uint32_t port, amp::ReplyTag const& tag,
                                   void const* payload, uint32_t len, uint32_t slot)
    {
        uint16_t const bound = amp::port_endpoint(port);
        // By index and not by handle: the bind's own reference holds the slot live, so there
        // is no generation for it to have lost.
        Endpoint* const e = kernel().endpoints.at(static_cast<int>(bound));
        if (e == nullptr or e->recv_holders == 0)
        {
            return false;
        }
        Thread* const w = wq_pop_highest(e->recv_waiters);
        if (w == nullptr)
        {
            return false; // no thread parked: refused now rather than held for one
        }

        // Once popped, this receiver is completed whatever follows: it is off its queue, so an
        // early return would leave it parked on nothing. A record it cannot be handed a
        // capability for is forgotten and the slot stays the taker's to release.
        uint32_t rcap = KCAP_INVALID;
        bool held = false;
        // An info-less recv has nowhere to be handed a capability, and write_recv_info answers
        // TRUE for it, so the seat is gated here rather than undone below.
        uint32_t record = amp::FAR_RECORD_NONE;
        if (w->ipc.badge_out != 0)
        {
            record = amp::inbound_seat(from, slot, tag);
        }
        if (record != amp::FAR_RECORD_NONE)
        {
            if (cap_install_far_reply(w, record, &rcap) == 0)
            {
                held = true;
            }
            else
            {
                amp::inbound_forget(record);
                rcap = KCAP_INVALID;
            }
        }

        size_t n = len;
        if (w->ipc.len < n)
        {
            n = w->ipc.len; // datagram truncation, as for any sender
        }
        intptr_t result = static_cast<intptr_t>(n);
        bool const copy_ok = kaccess_to_user(ipc_buf_space(w), w->ipc.buf, payload, n);
        // A CAPABILITY IS DISCLOSED ONLY BESIDE THE BYTES IT ANSWERS FOR. A receiver whose
        // buffer refused the payload never saw the request, so it is handed no obligation and
        // no handle to spend; endpoint_call keeps the same rule by minting after its copy.
        uint32_t disclose = rcap;
        if (not copy_ok)
        {
            disclose = KCAP_INVALID;
        }
        bool const info_ok =
            not endpoint_far_blind_take()
            and write_recv_info(user_space_of(w), w->ipc.badge_out, KOS_BADGE_NONE, disclose);
        // ONE FAULT, ONE ANSWER, and never an early return: the waiter is already off
        // recv_waiters, so returning here would leave it parked with nothing to wake it. Both
        // refusals are this node's own buffer fault on a message that arrived intact, so both
        // retract the mint and leave the far caller to dispatch_call's single publish site;
        // keeping the record instead would park that caller on a receiver holding an
        // obligation it was answered -KOS_EFAULT about. NOT ASSERTED, this being a masked
        // handler copying into a parked thread.
        if (not copy_ok or not info_ok)
        {
            result = -KOS_EFAULT;
            if (held)
            {
                bool const undone = cap_uninstall_far_reply(w, rcap, record);
                KICKOS_ASSERT(undone);
                (void)undone;
                amp::inbound_forget(record);
                held = false;
            }
            // One arrival owes ONE count however many of its copies were refused.
            amp::count_deliver_fault();
        }
        w->wait_result = result;
        sched::wake(w);
        return held;
    }

    // Caller holds IrqLock. Kernel init only, per N8: no capability authorises a crossing and
    // the partition is what states one.
    int amp_port_bind_local(Thread* c, uint32_t port, uint32_t* out_cap)
    {
        *out_cap = KCAP_INVALID;
        // PORT_REPLY names a CLASS and never a service, refused here as at the mint: a call
        // published on it lands in the reply ring, where a take answers CLASS and drops it.
        if (not amp::port_minted(amp::self(), port) or port == amp::PORT_REPLY)
        {
            return -KOS_EINVAL;
        }
        Endpoint* ep = nullptr;
        int const i = endpoint_slot_claim(&ep);
        if (i < 0)
        {
            return -KOS_ENOMEM;
        }
        // A LOCAL endpoint: far_node stays 0. What makes it a service for a peer is the port
        // bound to it.
        //
        // TWO REFERENCES: the installed capability's, and THE BIND'S OWN. port_bind stores a
        // slot INDEX with no generation beside it, and endpoint_far_call_deliver spends that
        // index directly, so the bind must hold the slot live: closing the seated cap
        // otherwise frees the slot while g_port_ep1 still names it, and the next
        // endpoint_create from any task lands there and answers a peer's callers.
        kernel().endpoint_refs[i] = 2;
        int const obj = kernel().endpoints.handle_for(i);
        int const rc = cap_install(c, obj, CapType::CAP_ENDPOINT, CAP_WAIT | CAP_SIGNAL, out_cap);
        if (rc != 0)
        {
            kernel().endpoint_refs[i] = 0;
            kernel().endpoints.free(obj);
            return rc;
        }
        ep->recv_holders = 1;
        amp::port_bind(port, static_cast<uint16_t>(i));
        return 0;
    }

    // The partition's port capabilities, seated into root before its first instruction. One
    // list (CONFIG_KICKOS_AMP_PORTS) names every crossing; an entry naming this node becomes a
    // local endpoint with the port bound to it, an entry naming another becomes a far endpoint.
    //
    // ENTRY i LANDS AT CAPABILITY INDEX KICKOS_CAP_FIRST_DYNAMIC + i, which is how
    // <kickos/amp.h> names the result without asking the kernel. That rests on this being the
    // FIRST dynamic install into root's freshly attached run, on the walk running in list
    // order, and on root's run carrying a slot per entry (cmake/cap_table.cmake sums one). An
    // install placed before this one shifts every constant positionally, and the slot check
    // below refuses to boot past it.
    void amp_ports_seat(Thread* root)
    {
        IrqLock lock;
        // Under the shared image self() is a core register while these constants are the
        // build's, so the two agreeing is a claim and not a restatement.
        KICKOS_ASSERT(amp::self() == amp::SELF_NODE);
        for (uint32_t i = 0; i < amp::PORT_COUNT; i++)
        {
            uint32_t const node = amp::PORT_NODE[i];
            uint32_t const port = amp::PORT_PORT[i];
            uint32_t cap = KCAP_INVALID;
            int rc = 0;
            if (node == amp::SELF_NODE)
            {
                rc = amp_port_bind_local(root, port, &cap);
            }
            else
            {
                rc = amp_endpoint_mint(root, node, port, &cap);
            }
            if (rc != 0)
            {
                kpanic(diag::kBootAmpPort);
            }
            // The whole handle and not only its index: <kickos/amp.h> names the capability as
            // a bare index, which is the handle only while the generation is zero. A run reused
            // with a bumped generation would answer the right slot under a handle no app can
            // spell.
            if (cap != KICKOS_CAP_FIRST_DYNAMIC + i)
            {
                kpanic(diag::kBootAmpSlot);
            }
        }
    }

#if defined(KICKOS_ENABLE_SELFTEST)
    bool endpoint_far_reply_route(amp::ReplyTag* out_tag, uint32_t* out_node)
    {
        ThreadPool& tp = kernel().threads;
        for (int i = 0; i < tp.next; i++)
        {
            Thread* const t = &tp.slots[i];
            Endpoint const* const e = t->wait_far_endpoint();
            if (e == nullptr or t->state != ThreadState::BLOCKED)
            {
                continue;
            }
            out_tag->thread = tp.handle_for(i);
            out_tag->seq = amp::reply_seq(t->call_seq);
            *out_node = endpoint_far_node(e);
            return true;
        }
        return false;
    }
#endif
#endif
}
