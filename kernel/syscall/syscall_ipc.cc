// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Enter send, recv and call without holding IrqLock. The resume barrier needs
// interrupts enabled; an outer lock keeps BASEPRI raised and livelocks ARM.

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

        // Defer one local reschedule until the fused operation finishes or parks.
        // Keep the highest-priority woken thread for that reschedule and announce
        // other woken threads to eligible peers.
        // The destructor completes nonparking paths; take() transfers completion to
        // wq_block. Do not call a nonreturning exit while this guard is live: it
        // would skip the destructor. Check park_cancel_pending before constructing it.
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

            // Keep the highest-priority thread for rescheduling; announce the other now.
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

        // Allocate an endpoint with all fields reset to their defaults.
        // Return -1 without changing *out if the pool is full.
        [[nodiscard]] int endpoint_slot_claim(Endpoint** out)
        {
            int const i = kernel().endpoints.alloc();
            Endpoint* const ep = kernel().endpoints.at(i); // at(-1) returns null.
            if (ep == nullptr)
            {
                return -1;
            }
            *ep = Endpoint{};
            *out = ep;
            return i;
        }

#if KICKOS_AMP_NODE
        // Copy the caller buffer through a kernel staging buffer to the far endpoint.
        // Return bytes sent or the mapped window error. tag identifies the reply route;
        // REPLY_TAG_NONE means the sender does not park. The caller holds IrqLock.
        // The stage uses KOS_EP_MSG_MAX bytes of syscall stack.
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

    // Rollback both endpoint_refs (all capabilities) and recv_holders (WAIT capabilities).
    int endpoint_create(uint32_t* out_cap)
    {
        IrqLock lock;
        *out_cap = KCAP_INVALID;
        Thread* c = sched::current();
        if (c == nullptr)
        {
            return -KOS_EPERM;
        }
        // Check the task limit before allocating a pool slot.
        if (not task_object_admit(CapType::CAP_ENDPOINT, c->task))
        {
            return -KOS_EOVERFLOW;
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
    // Keep the wire sequence as wide as call_seq to prevent premature wraparound.
    static_assert(amp::REPLY_SEQ_MASK == (1u << (8u * sizeof(Thread::call_seq))) - 1u,
                  "the reply tag's validated width must be the whole of Thread::call_seq");
#endif

#if KICKOS_AMP_NODE
    // Caller holds IrqLock and must check privilege.
    int amp_endpoint_mint(Thread* c, uint32_t node, uint32_t port, uint32_t* out_cap)
    {
        *out_cap = KCAP_INVALID;
        // port_minted validates both arguments. PORT_REPLY is reserved for replies.
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
        // Far endpoints have CAP_SIGNAL only, so receive and server binding are refused.
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

    // Create an endpoint in another node. Only privileged callers may use crossings
    // listed in the static configuration; no capability grants a crossing (N8).
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

    // Return bytes sent or -KOS_E*. timeout_us bounds only the parked wait.
    // Endpoint death, timeout and cancellation send no data.
    // Far endpoints never park; a full peer ring returns -KOS_EBUSY.
    int32_t endpoint_send(uint32_t cap, uintptr_t buf, size_t len, uint32_t timeout_us)
    {
        if (len > KOS_EP_MSG_MAX)
        {
            return -KOS_EINVAL;
        }
        if (not user_readable_ok(buf, len))
        {
            return -KOS_EFAULT; // Validate in caller context.
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
            // Handle far endpoints before checking recv_holders: they have no local receiver.
            if (endpoint_is_far(e))
            {
                return far_publish(c, e, buf, len, amp::REPLY_TAG_NONE);
            }
#endif
            if (e->recv_holders == 0)
            {
                return -KOS_EPIPE; // No receiver remains.
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
                    // Wake the removed receiver with EFAULT; a partial copy is not a zero-length message.
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
            c->call_state = CALL_NONE; // Plain send, not call.
            epoch = c->switch_count;
            park_deadline_arm(c, timeout_us);
            wq_block(e->send_waiters, WAIT_EP_SEND, e);
        }
        wq_confirm_resume(c, epoch);
        // n (>= 0), -KOS_EPIPE (last receiver left), -KOS_ETIMEDOUT (ktime_on_timer), or
        // -KOS_ECANCELED (this parked caller was cancelled)
        return static_cast<int32_t>(c->wait_result);
    }

    // Send a kernel fault record without parking. Use the published endpoint reference
    // and a kernel buffer; reject a busy or missing driver.
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
            n = 0;
        }
        // Wake the removed waiter even on error; it is no longer on recv_waiters.
        if (not write_recv_info(user_space_of(w), w->ipc.badge_out, KOS_BADGE_NONE, KCAP_INVALID))
        {
            n = 0;
        }
        w->wait_result = static_cast<intptr_t>(n);
        sched::wake(w);
        return static_cast<int32_t>(n);
    }

    // Receive with IrqLock held and cap_len clamped. badge_out is zero for a
    // receive without metadata. Add wakes to dw; parking takes its pending wake.
    // Returns bytes received or -KOS_E*. On park, sets parked and epoch; the caller
    // must run wq_confirm_resume before reading wait_result.
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
        endpoint_server_set(e, c); // Set the target for server priority donation.
        // Defer rescheduling: changing current() here would make wq_block park the
        // woken caller instead of this receiver.
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
                // A receive without metadata cannot return a reply cap. Reject the call
                // and continue looking for plain sends.
                if (badge_out == 0)
                {
                    s->call_state = CALL_NONE; // clear before waking
                    s->wait_result = -KOS_ENOSYS;
                    // Remove this rejected caller's priority donation.
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
                // Check reply-cap capacity before committing; later plain sends may still fit.
                if (not cap_can_take_reply(c))
                {
                    s->call_state = CALL_NONE;
                    s->wait_result = -KOS_EMFILE; // server table is full
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
                    n = cap_len;
                }
                bool ok = ep_copy(user_space_of(c), buf, ipc_buf_space(s), s->ipc.buf, n);
                if (ok)
                {
                    uint32_t rcap = KCAP_INVALID;
                    // Allocate outside the assert so release builds also execute it.
                    int const minted = cap_install_reply(c, s, &rcap);
                    KICKOS_ASSERT(minted == 0);
                    ok = write_recv_info(user_space_of(c), badge_out, KOS_BADGE_NONE, rcap);
                    if (not ok)
                    {
                        // Revoke the undisclosed reply cap and fail the caller.
                        bool const undone = cap_uninstall_reply(c, rcap, s);
                        KICKOS_ASSERT(undone);
                        (void)undone;
                    }
                }
                if (not ok)
                {
                    // Stop on copy failure: if our buffer is invalid, continuing would fail
                    // every queued sender.
                    s->call_state = CALL_NONE; // clear before waking
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
                // Inherit the caller's priority for this transaction.
                if (s->prio > c->prio)
                {
                    sched::set_prio(c, s->prio);
                }
                return static_cast<int>(n);
            }
            size_t n = s->ipc.len;
            if (cap_len < n)
            {
                n = cap_len;
            }
            if (not ep_copy(user_space_of(c), buf, ipc_buf_space(s), s->ipc.buf, n))
            {
                // Fail both ends and stop; our buffer may be the invalid one.
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
        // Transfer the pending wake to the park's reschedule, including peer notification.
        wq_block(e->recv_waiters, WAIT_EP_RECV, e, dw->take());
        KICKOS_BENCH_SPAN(PH_RECV_PARK, bm_rpark);
        *parked = true;
        KICKOS_BENCH_SPAN(PH_RECV_LOCKED, bm_rlocked);
        return 0;
    }

    // Send a request and receive its reply in one buffer. Returns reply bytes
    // or -KOS_E*: EINVAL for length, EFAULT for buffers, EBADF/EPERM for capability
    // access, EPIPE for endpoint/server death, EMFILE for server reply-cap capacity,
    // ENOSYS for a receiver without metadata, ETIMEDOUT, or ECANCELED.
    // One deadline covers both send and reply waits. Far calls wait only for a
    // reply and return EBUSY without side effects if the peer ring is full.
    int32_t endpoint_call(uint32_t cap, uintptr_t buf, size_t send_len, size_t recv_cap,
                          uint32_t timeout_us)
    {
        KICKOS_BENCH_MARK(bm_total);
        if (send_len > KOS_EP_MSG_MAX)
        {
            return -KOS_EINVAL;
        }
        if (recv_cap > KOS_EP_MSG_MAX)
        {
            recv_cap = KOS_EP_MSG_MAX; // Clamp reply capacity.
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
            // Check cancellation before removing a receiver or creating reply state.
            // A nonreturning exit after those changes would strand the receiver.
            if (park_cancel_pending(c))
            {
                sched::exit_current(KOS_EXIT_CANCELLED, sched::EXIT_RETURN); // noreturn
            }
            KICKOS_BENCH_MARK(bm_locked);
            // Calibrate under the same interrupt mask as the measured phases.
            // PH_NEST includes the accumulator work excluded from PH_NULL.
            KICKOS_BENCH_MARK(bm_nest);
            KICKOS_BENCH_MARK(bm_null);
            KICKOS_BENCH_SPAN(PH_NULL, bm_null);
            KICKOS_BENCH_SPAN(PH_NEST, bm_nest);
#if KICKOS_BENCH
            // Measure a nested acquisition; an outer IrqLock is already held.
            // Compile it out in non-benchmark builds because it performs real lock operations.
            KICKOS_BENCH_MARK(bm_nest_lock);
            {
                IrqLock nested;
            }
            KICKOS_BENCH_SPAN(PH_NEST_LOCK, bm_nest_lock);
#endif
            int err = 0;
            KICKOS_BENCH_MARK(bm_resolve);
            Endpoint* e = static_cast<Endpoint*>(
                cap_resolve_e(c, cap, CapType::CAP_ENDPOINT, CAP_SIGNAL, &err));
            KICKOS_BENCH_SPAN(PH_CALL_RESOLVE, bm_resolve);
            if (e == nullptr)
            {
                return -err; // EBADF (bad cap) or EPERM (no SIGNAL right)
            }
            // Handle far endpoints before checking recv_holders: they have no local receiver.
            bool const far = endpoint_is_far(e);
            if (not far and e->recv_holders == 0)
            {
                return -KOS_EPIPE; // No receiver remains.
            }
            KICKOS_BENCH_MARK(bm_peek);
            Thread* w = wq_peek_highest(e->recv_waiters);
            KICKOS_BENCH_SPAN(PH_CALL_PEEK, bm_peek);
#if KICKOS_AMP_NODE
            if (far)
            {
                int const idx = kernel().threads.index_of(c);
                KICKOS_ASSERT(idx >= 0); // The idle thread is outside the pool and cannot call.
                // Commit call_seq only after successful publication.
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
                // Cross-node calls do not donate priority to the peer scheduler (N6e).
                epoch = c->switch_count;
                park_queueless(c, WAIT_EP_FAR_REPLY, e);
                park_deadline_arm(c, timeout_us);
                // No wake schedules this park; request a switch explicitly.
                sched::reschedule();
            }
            else
#endif
            if (w != nullptr)
            {
                // Probe before removing the receiver so a refusal cannot strand it.
                if (w->ipc.badge_out == 0)
                {
                    return -KOS_ENOSYS; // Calls require receive metadata.
                }
                if (w->dying)
                {
                    // Do not mint into a table being destroyed. Its cleanup may already have
                    // passed the slot, and cleanup releases IrqLock between chunks.
                    return -KOS_EPIPE;
                }
                KICKOS_BENCH_MARK(bm_probe);
                if (not cap_can_take_reply(w))
                {
                    // Check the receiver table and reply limit before changing state.
                    return -KOS_EMFILE;
                }
                KICKOS_BENCH_SPAN(PH_CALL_PROBE, bm_probe);
                KICKOS_BENCH_MARK(bm_pop);
                (void)wq_pop_highest(e->recv_waiters); // IrqLock keeps w at the head.
                KICKOS_BENCH_SPAN(PH_CALL_POP, bm_pop);
                size_t n = send_len;
                if (w->ipc.len < n)
                {
                    n = w->ipc.len; // receiver-side request truncation
                }
                KICKOS_BENCH_MARK(bm_copy);
                if (not ep_copy(ipc_buf_space(w), w->ipc.buf, user_space_of(c), buf, n))
                {
                    // The caller is unchanged. Wake the receiver because it has left recv_waiters.
                    w->wait_result = -KOS_EFAULT;
                    sched::wake(w);
                    return -KOS_EFAULT;
                }
                KICKOS_BENCH_SPAN(PH_CALL_COPY, bm_copy);
                c->call_seq++; // Advance before packing the reply capability.
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
                    // Revoke the undisclosed capability and fail both ends.
                    // The receiver has left recv_waiters, but the caller has not parked.
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
                // Donate to the server before waking it.
                if (c->prio > w->prio)
                {
                    KICKOS_BENCH_MARK(bm_donate);
                    sched::set_prio(w, c->prio);
                    KICKOS_BENCH_SPAN(PH_CALL_DONATE, bm_donate);
                }
                epoch = c->switch_count;
                KICKOS_BENCH_MARK(bm_park);
                park_queueless(c, WAIT_EP_REPLY, w);
                reply_donor_park(w, c); // Off the ready queue; link can be reused.
                // Arm the reply deadline before the wake can switch threads.
                park_deadline_arm(c, timeout_us);
                KICKOS_BENCH_SPAN(PH_CALL_PARK, bm_park);
                KICKOS_BENCH_MARK(bm_wake);
                sched::wake(w);
                KICKOS_BENCH_SPAN(PH_CALL_WAKE, bm_wake);
                // Close inside IrqLock so a deferred switch does not add the round trip.
                // Keep fastpath and slowpath samples separate.
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
                c->call_seq++; // Packed when the receiver removes this call.
                c->call_state = CALL_SEND_WAIT;
                // Boost the bound server before parking.
                if (e->server != nullptr and c->prio > e->server->prio)
                {
                    KICKOS_BENCH_MARK(bm_sdonate);
                    sched::set_prio(e->server, c->prio);
                    KICKOS_BENCH_SPAN(PH_CALL_SLOW_DONATE, bm_sdonate);
                }
                epoch = c->switch_count;
                KICKOS_BENCH_MARK(bm_spark);
                // Keep this deadline when the caller moves from send_waiters to reply_waiters.
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
        c->call_state = CALL_NONE; // Clear call state on every return path.
        KICKOS_BENCH_SPAN(PH_CALL_RESUME, bm_resume);
        // reply bytes, or -KOS_EPIPE/-KOS_EMFILE/-KOS_ENOSYS/-KOS_ETIMEDOUT/-KOS_ECANCELED
        return static_cast<int32_t>(c->wait_result);
    }

    // Reply with IrqLock held. Consumes the reply cap and adds wakes to dw.
    // Returns 0 or -KOS_E* (EBADF, EFAULT, or ESRCH).
    // Set answered only for a local caller wake; far replies do not contribute
    // to the local benchmark phases.
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
        // Reply to a caller in another kernel using the reserved reply-record range.
        // The wire carries bytes and length, not errno; protocols must distinguish
        // empty replies from service failure in their payload.
        if (ThreadPool::far_reply_is(static_cast<uint32_t>(cap_reply_handle(*e))))
        {
            uint32_t const record =
                ThreadPool::far_reply_record(static_cast<uint32_t>(cap_reply_handle(*e)));
            // Consume the reply capability on every exit.
            e->gen++;
            e->type = static_cast<uint8_t>(CapType::CAP_EMPTY);
            e->rights = 0;
            cap_run_free_release(c->caps, reply_cap & KCAP_INDEX_MASK, e, &c->cap_free_head);
            cap_reply_released(c);
            uint8_t stage[KOS_EP_MSG_MAX];
            if (not kaccess_from_user(stage, user_space_of(c), buf, len))
            {
                // Release the peer call slot even if the service buffer is invalid.
                amp::inbound_reply(record, stage, 0u);
                return -KOS_EFAULT;
            }
            amp::inbound_reply(record, stage, static_cast<uint32_t>(len));
            return 0;
        }
#endif
        Thread* caller = cap_reply_caller(*e); // Validate before consuming the capability.
        // cap_run_free_release writes the free-list links over `obj`, so it must follow the
        // resolve above.
        e->gen++;
        e->type = static_cast<uint8_t>(CapType::CAP_EMPTY);
        e->rights = 0;
        cap_run_free_release(c->caps, reply_cap & KCAP_INDEX_MASK, e, &c->cap_free_head);
        cap_reply_released(c);
        // Unlink the donor before recomputing priority. A caller now waiting on another
        // server no longer belongs to this reply.
        if (caller != nullptr and not reply_donor_unpark(c, caller))
        {
            caller = nullptr;
        }
        KICKOS_BENCH_SPAN(PH_REPLY_LOOKUP, bm_lookup);
        if (caller == nullptr)
        {
            // Timeout leaves CALL_NONE and an outstanding server capability.
            // Remove its donation even though the caller is already awake.
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
        // The capability is consumed and the caller is off its queue; wake it on error.
        caller->wait_result = static_cast<intptr_t>(n);
        if (not ok)
        {
            caller->wait_result = -KOS_EFAULT;
        }
        caller->call_state = CALL_NONE;
        KICKOS_BENCH_MARK(bm_funnel);
        // Restore priority through the donation chain.
        sched::set_prio(c, thread_effective_prio(c));
        KICKOS_BENCH_SPAN(PH_REPLY_FUNNEL, bm_funnel);
        *answered = true;
        KICKOS_BENCH_MARK(bm_wake);
        // Defer rescheduling so a fused receive can park first.
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
            // Complete the deferred wake before closing PH_REPLY_TOTAL.
            DeferredWake dw;
            rc = endpoint_reply_locked(c, reply_cap, buf, len, &dw, &answered);
        }
        if (answered)
        {
            // Close before lock release can trigger a pending switch.
            KICKOS_BENCH_SPAN(PH_REPLY_TOTAL, bm_total);
        }
        return rc;
    }

    // Reply and receive using one syscall, lock, and shared buffer. The reply
    // is copied before the receive can overwrite it. lens packs two 9-bit lengths.
    // Defer wakes until receiving finishes or parks, so a resumed client can
    // find the server waiting for its next request.
    // ESRCH still allows receiving: that transaction has already ended. EBADF
    // and EFAULT abort receiving so its result cannot hide the reply error.
    // After server-buffer validation, a reply EFAULT can be a client-buffer fault;
    // service loops must handle it without terminating the shared service.
    int32_t endpoint_reply_recv(uint32_t reply_cap, uintptr_t buf, uintptr_t lens,
                                uintptr_t opts)
    {
        KICKOS_BENCH_MARK(bm_frtotal);
        size_t reply_len = kos_call_lens_send(lens);
        size_t recv_cap = kos_call_lens_recv(lens);
        // Clamp both lengths to KOS_EP_MSG_MAX. Packing saturates at 511, so an
        // oversized length reaches this clamp instead of wrapping.
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
            return -KOS_EINVAL; // reject misalignment before privileged accesses
        }
        Thread* c = sched::current();
        if (c == nullptr)
        {
            return -KOS_EPERM;
        }
        if (not user_readable_and_writable_ok(opts, sizeof(kos_reply_recv_opts),
                                              sizeof(kos_reply_recv_opts)))
        {
            return -KOS_EFAULT;
        }
        // Snapshot the user-writable inputs once before parking, including the
        // in/out notify mask. info is output-only.
        kos_reply_recv_opts in = {};
        if (not kaccess_from_user(&in, user_space_of(c), opts,
                                  offsetof(kos_reply_recv_opts, info)))
        {
            return -KOS_EFAULT;
        }
        uint32_t const admit = in.notify;
        uint32_t notify_bits = 0;
        // Run the remaining paths through one write-back of consumed notifications.
        // Use a callable so early returns still reach it. A destructor cannot replace
        // the return code with EFAULT if write-back fails.
        auto const served = [&]() -> int32_t
        {
            if ((in.flags & ~KOS_RECV_NO_INFO) != 0)
            {
                return -KOS_EINVAL;
            }
            // Validate the shared buffer for both reply reads and receive writes.
            if (not user_readable_and_writable_ok(buf, reply_len, recv_cap))
            {
                return -KOS_EFAULT;
            }
            uintptr_t badge_out = opts + offsetof(kos_reply_recv_opts, info);
            if ((in.flags & KOS_RECV_NO_INFO) != 0)
            {
                badge_out = 0; // no metadata; reject calls
            }
            uint32_t epoch = 0;
            bool parked = false;
            int32_t rc = 0;
            uint32_t opened = 0;
            bool reply_refused = false;
            {
                IrqLock lock;
                if (park_cancel_pending(c))
                {
                    sched::exit_current(KOS_EXIT_CANCELLED, sched::EXIT_RETURN); // noreturn
                }
                {
                    // Complete the wake before closing the total, while IrqLock is held.
                    DeferredWake dw;
                    // Only standalone replies use this reply-phase timestamp.
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
                        // Accept only this thread's bound lines and rearm consumed events.
                        // The matching leave consumes bits and marks them for the next rearm.
                        opened = irq_notify_wait_enter(c, admit);
                        if ((c->notify_pending & opened) != 0u)
                        {
                            // Return the notification now; a queued message can be received next time.
                            rc = -KOS_ENOTIFY;
                        }
                        else
                        {
                            rc = endpoint_recv_locked(c, in.ep, buf, recv_cap, badge_out,
                                                      in.timeout_us, &dw, &parked, &epoch);
                        }
                        if (not parked)
                        {
                            // No park occurred, so pending bits can be consumed under this lock.
                            notify_bits = irq_notify_wait_leave(c, opened);
                        }
                    }
                }
                if (not reply_refused)
                {
                    // Close before lock release so deferred-switch targets exclude the wait.
                    // Do not record reply refusals.
                    KICKOS_BENCH_SPAN(PH_REPLY_RECV_TOTAL, bm_frtotal);
                }
            }
            if (parked)
            {
                KICKOS_BENCH_MARK(bm_frtail);
                wq_confirm_resume(c, epoch);
                rc = static_cast<int32_t>(c->wait_result);
                {
                    // Consume bits after the resume barrier. A deferred switch may not have
                    // run when wq_block returns. The barrier needs interrupts enabled so that
                    // switch can run before this thread reads its wake result.
                    IrqLock lock;
                    notify_bits = irq_notify_wait_leave(c, opened);
                }
                KICKOS_BENCH_SPAN(PH_REPLY_RECV_TAIL, bm_frtail);
            }
            return rc;
        };
        int32_t const rc = served();
        // Write consumed bits on every path after the input snapshot, including errors.
        // Skip when the input mask was zero: it already reports zero consumed bits.
        // A failed write-back returns EFAULT even if a message was received.
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

    // Called from the doorbell handler with local interrupts masked.
    bool endpoint_far_reply_deliver(uint32_t from, amp::ReplyTag const& tag, void const* payload,
                                    uint32_t len)
    {
        Thread* caller =
            cap_reply_thread(tag.thread, amp::reply_seq(tag.seq), amp::REPLY_SEQ_MASK);
        if (caller == nullptr)
        {
            return false;
        }
        // Match the source node as well as the parked call state.
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
        // Report copy failure through the local TCB, not an assertion or wire errno.
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

    // Deliver a call from another kernel. Runs in the doorbell handler with
    // local interrupts masked; takes no lock.
    // True retains the call slot until reply. False releases it immediately and
    // requires dispatch_call to send a zero-length tagged reply, so the far
    // caller cannot remain blocked. Do not send a second reply here.
    bool endpoint_far_call_deliver(uint32_t from, uint32_t port, amp::ReplyTag const& tag,
                                   void const* payload, uint32_t len, uint32_t slot)
    {
        uint16_t const bound = amp::port_endpoint(port);
        // The binding holds the endpoint slot alive, so lookup uses its index.
        Endpoint* const e = kernel().endpoints.at(static_cast<int>(bound));
        if (e == nullptr or e->recv_holders == 0)
        {
            return false;
        }
        Thread* const w = wq_pop_highest(e->recv_waiters);
        if (w == nullptr)
        {
            return false; // No receiver is waiting.
        }

        // Always complete a receiver removed from its queue. If it cannot receive a
        // reply capability, discard the record and let the caller release the slot.
        uint32_t rcap = KCAP_INVALID;
        bool held = false;
        // Require metadata space before creating a reply capability.
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
        // Mint a reply capability only after the request payload has been copied.
        uint32_t disclose = rcap;
        if (not copy_ok)
        {
            disclose = KCAP_INVALID;
        }
        bool const info_ok =
            not endpoint_far_blind_take()
            and write_recv_info(user_space_of(w), w->ipc.badge_out, KOS_BADGE_NONE, disclose);
        // On either copy failure, revoke the mint and wake the removed receiver with
        // EFAULT. Return false so dispatch_call sends the single failure reply.
        // Do not assert on a buffer fault in this masked handler.
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
            // Count one arrival even if both copies fail.
            amp::count_deliver_fault();
        }
        w->wait_result = result;
        sched::wake(w);
        return held;
    }

    // Kernel initialization only, with IrqLock held. N8 permits configured crossings.
    int amp_port_bind_local(Thread* c, uint32_t port, uint32_t* out_cap)
    {
        *out_cap = KCAP_INVALID;
        // PORT_REPLY is reserved for replies, not service calls.
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
        // Create a local endpoint and bind it to the peer service port.
        // Keep separate references for the capability and binding: port_bind stores
        // a slot index, so the binding must keep that slot alive after the cap closes.
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

    // Install CONFIG_KICKOS_AMP_PORTS capabilities into root before it runs.
    // Local-node entries create local endpoints; other entries create far endpoints.
    // Entry i must occupy KICKOS_CAP_FIRST_DYNAMIC + i. Install in list order
    // before any other dynamic capability, with enough slots for every entry.
    // The slot check rejects a boot that breaks this ABI layout.
    void amp_ports_seat(Thread* root)
    {
        IrqLock lock;
        // Verify that this core matches the shared image configuration.
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
            // Check the full handle. The public AMP ABI uses a bare index and therefore
            // requires generation zero.
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
