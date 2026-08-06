// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Synchronous IPC: endpoint create plus the send/recv/call/reply rendezvous. send, recv
// and call MUST be entered with no caller-held IrqLock: each takes its own for the
// resolve/deliver/park then releases it before the resume barrier, and a spanning caller
// lock would keep BASEPRI raised across wq_confirm_resume and livelock ARM.

#include <kickos/cap.h>
#include <kickos/endpoint.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/thread.h>

#include <kickos/sys/abi.h>
#include <kickos/sys/errno.h>

#include "syscall_internal.h"

namespace kickos
{
    namespace
    {
        // Endpoint badge delivered to a receiver. Stage i has no per-endpoint badge
        // scheme yet, so every message carries this. Distinct from the "no out-ptr"
        // markers (ipc.badge_out == 0), which mean the receiver asked for no badge.
        constexpr uint32_t KOS_BADGE_NONE = 0;
    }

    // --- Endpoint capability (IPC rendezvous; mirrors sem_create) ---------------
    // An endpoint lives in the global pool (slotpool.h); a task names it by a
    // per-task CAP_ENDPOINT capability. The creator cap carries full rights
    // (WAIT|SIGNAL|TRANSFER); send needs CAP_SIGNAL, recv needs CAP_WAIT. Two
    // counters init visibly paired: endpoint_refs (all caps) and recv_holders
    // (WAIT-bearing caps). Rollback on a full table unwinds BOTH.
    int endpoint_create(uint32_t* out_cap)
    {
        IrqLock lock;
        *out_cap = KCAP_INVALID;
        Thread* c = sched::current();
        if (c == nullptr)
        {
            return -KOS_EPERM; // no caller context (defensive)
        }
        int const i = kernel().endpoints.alloc();
        if (i < 0)
        {
            return -KOS_ENOMEM; // endpoint pool exhausted
        }
        Endpoint* ep = kernel().endpoints.at(i);
        ep->send_waiters = List{};
        ep->recv_waiters = List{};
        ep->recv_holders = 1;         // creator holds a WAIT-bearing cap
        ep->server = nullptr;         // no conventional receiver until the first recv
        // A reused slot keeps its last occupant's link, and "server null implies unlinked"
        // must hold from the moment the slot is handed out.
        ep->next_served = EP_SERVED_NONE;
        kernel().endpoint_refs[i] = 1; // this creator's cap is the first reference
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

    // Synchronous send: rendezvous with a parked receiver (deliver now) or park.
    // FULLY LOCKLESS from dispatch (see design section 3): a caller IrqLock spanning
    // this would keep BASEPRI raised across wq_confirm_resume and livelock ARM.
    // Returns bytes transferred (>= 0), or -KOS_E* (EINVAL oversize, EFAULT bad buffer,
    // EBADF/EPERM bad cap or missing SIGNAL, EPIPE dead endpoint / last receiver left).
    int32_t endpoint_send(uint32_t cap, uintptr_t buf, size_t len)
    {
        if (len > KOS_EP_MSG_MAX)
        {
            return -KOS_EINVAL; // F4: reject oversize, never silently clamp
        }
        if (not user_readable_ok(buf, len))
        {
            return -KOS_EFAULT; // sender's own buffer, checked once in caller context
        }
        Thread* c = sched::current();
        if (c == nullptr)
        {
            return -KOS_EPERM; // no caller context (defensive)
        }
        uint64_t epoch = 0;
        {
            IrqLock lock;
            int err = 0;
            Endpoint* e = static_cast<Endpoint*>(
                cap_resolve_e(c, cap, CapType::CAP_ENDPOINT, CAP_SIGNAL, &err));
            if (e == nullptr)
            {
                return -err; // EBADF (bad cap) or EPERM (no SIGNAL right)
            }
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
                    n = w->ipc.len; // receiver-side datagram truncation (not an error)
                }
                ep_copy(w->ipc.buf, buf, n); // sender-ctx copy into the parked receiver's buffer
                // Plain send: no reply cap (this is not a call).
                write_recv_info(w->ipc.badge_out, KOS_BADGE_NONE, KCAP_INVALID);
                w->wait_result = static_cast<intptr_t>(n);
                sched::wake(w);
                return static_cast<int>(n); // did not block: no resume barrier
            }
            c->ipc.buf = buf;
            c->ipc.len = len;
            c->ipc.badge_out = 0;
            c->call_state = CALL_NONE; // B1: a plain sender is never a call
            epoch = c->switch_count;
            wq_block(e->send_waiters);
        }
        wq_confirm_resume(c, epoch);
        return static_cast<int32_t>(c->wait_result); // n (>= 0), or -KOS_EPIPE (last receiver left)
    }

    // Synchronous recv: take from a parked sender (copy now) or park. FULLY LOCKLESS
    // from dispatch, same reason as endpoint_send. Returns bytes received (>= 0), or
    // -KOS_E* (EFAULT bad buffer/out-ptr, EINVAL misaligned badge, EBADF/EPERM bad cap or
    // missing WAIT). n == 0 is a VALID zero-length signal, not an error.
    int32_t endpoint_recv(uint32_t cap, uintptr_t buf, size_t cap_len, uintptr_t badge_out)
    {
        if (cap_len > KOS_EP_MSG_MAX)
        {
            cap_len = KOS_EP_MSG_MAX; // capacity clamp is harmless
        }
        if (not user_writable_ok(buf, cap_len))
        {
            return -KOS_EFAULT;
        }
        // The out-ptr delivers a kos_recv_info (8 bytes, 4-aligned), not a bare badge u32.
        // badge_out == 0 means an info-less recv, which cannot host a call.
        if (badge_out != 0
            and ((badge_out & (alignof(uint32_t) - 1)) != 0
                 or not user_writable_ok(badge_out, sizeof(kos_recv_info))))
        {
            // Misalignment is a malformed arg (EINVAL); an unowned out-ptr is EFAULT.
            if ((badge_out & (alignof(uint32_t) - 1)) != 0)
            {
                return -KOS_EINVAL; // alignment load-bearing for the privileged store below
            }
            return -KOS_EFAULT;
        }
        Thread* c = sched::current();
        if (c == nullptr)
        {
            return -KOS_EPERM; // no caller context (defensive)
        }
        uint64_t epoch = 0;
        {
            IrqLock lock;
            int err = 0;
            Endpoint* e = static_cast<Endpoint*>(
                cap_resolve_e(c, cap, CapType::CAP_ENDPOINT, CAP_WAIT, &err));
            if (e == nullptr)
            {
                return -err; // EBADF (bad cap) or EPERM (no WAIT right)
            }
            endpoint_server_set(e, c); // the conventional receiver (D2 boost target)
            while (true)
            {
                Thread* s = wq_pop_highest(e->send_waiters);
                if (s == nullptr)
                {
                    break; // nobody parked: fall through and park as a receiver
                }
                if (s->call_state == CALL_SEND_WAIT)
                {
                    // A slow-path caller needs a reply cap minted into OUR table, which an
                    // info-less recv cannot deliver: reject this caller with ENOSYS and
                    // keep scanning for plain traffic behind it.
                    if (badge_out == 0)
                    {
                        s->call_state = CALL_NONE; // B1: clear before waking
                        s->wait_result = -KOS_ENOSYS;
                        // Revert the D2 boost this bounced caller may have pinned on us
                        // (it is off-queue with CALL_NONE, so the funnel excludes it).
                        uint8_t const np = thread_effective_prio(c);
                        if (np != c->prio)
                        {
                            sched::set_prio(c, np);
                        }
                        sched::wake(s);
                        continue;
                    }
                    // B3: probe the mint before committing (our table may be full, or we
                    // may be at our reply bound; a plain sender behind it can still be
                    // served, so keep scanning).
                    if (not cap_can_take_reply(c))
                    {
                        s->call_state = CALL_NONE;
                        s->wait_result = -KOS_EMFILE; // OUR table, reported to the caller
                        uint8_t const np = thread_effective_prio(c);
                        if (np != c->prio)
                        {
                            sched::set_prio(c, np);
                        }
                        sched::wake(s);
                        continue;
                    }
                    size_t n = s->ipc.len;
                    if (cap_len < n)
                    {
                        n = cap_len; // truncate the request into our capacity
                    }
                    ep_copy(buf, s->ipc.buf, n);
                    uint32_t rcap = KCAP_INVALID;
                    // Not inside the assert: a compiled-out condition would drop the mint.
                    int const minted = cap_install_reply(c, s, &rcap);
                    KICKOS_ASSERT(minted == 0); // cap_can_take_reply probed this above
                    write_recv_info(badge_out, KOS_BADGE_NONE, rcap);
                    // Repurpose the caller's ipc to the reply target (in-place buffer,
                    // reply capacity); it re-parks on OUR reply-donor list.
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
                // Plain parked sender (CALL_NONE): today's behavior verbatim.
                size_t n = s->ipc.len;
                if (cap_len < n)
                {
                    n = cap_len; // truncate into the receiver's capacity
                }
                ep_copy(buf, s->ipc.buf, n); // receiver-ctx copy from the parked sender
                write_recv_info(badge_out, KOS_BADGE_NONE, KCAP_INVALID);
                s->wait_result = static_cast<intptr_t>(n);
                sched::wake(s);
                return static_cast<int>(n);
            }
            c->ipc.buf = buf;
            c->ipc.len = cap_len;
            c->ipc.badge_out = badge_out;
            epoch = c->switch_count;
            wq_block(e->recv_waiters);
        }
        wq_confirm_resume(c, epoch);
        return static_cast<int32_t>(c->wait_result);
    }

    // Synchronous call: deliver a request to the endpoint's server, park until it
    // replies. In-place buffer (request out, reply back). FULLY LOCKLESS from dispatch
    // like SEND/RECV. Returns reply bytes (>= 0), or -KOS_E* (EINVAL oversize, EFAULT
    // bad buffer, EBADF/EPERM bad cap or no SIGNAL, EPIPE dead endpoint or server died,
    // EMFILE the SERVER's table is full or it is at its inbound reply bound, ENOSYS
    // server took an info-less recv).
    int32_t endpoint_call(uint32_t cap, uintptr_t buf, size_t send_len, size_t recv_cap)
    {
        if (send_len > KOS_EP_MSG_MAX)
        {
            return -KOS_EINVAL; // reject oversize request, like send
        }
        if (recv_cap > KOS_EP_MSG_MAX)
        {
            recv_cap = KOS_EP_MSG_MAX; // reply capacity clamp is harmless
        }
        // The one buffer is read (request) then written (reply): validate both here,
        // in caller context, once.
        if (not user_readable_ok(buf, send_len) or not user_writable_ok(buf, recv_cap))
        {
            return -KOS_EFAULT;
        }
        Thread* c = sched::current();
        if (c == nullptr)
        {
            return -KOS_EPERM; // no caller context (defensive)
        }
        uint64_t epoch = 0;
        {
            IrqLock lock;
            int err = 0;
            Endpoint* e = static_cast<Endpoint*>(
                cap_resolve_e(c, cap, CapType::CAP_ENDPOINT, CAP_SIGNAL, &err));
            if (e == nullptr)
            {
                return -err; // EBADF (bad cap) or EPERM (no SIGNAL right)
            }
            if (e->recv_holders == 0)
            {
                return -KOS_EPIPE; // dead endpoint: no receiver can ever exist
            }
            Thread* w = wq_peek_highest(e->recv_waiters);
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
                    // have passed the slot, so the mint would outlive the thread and
                    // strand this caller. Unreachable today, since a thread reaches
                    // exit_current from RUNNING and is thus parked on no recv queue, but
                    // the sweep now drops IrqLock between chunks and this check is what
                    // keeps it unreachable.
                    return -KOS_EPIPE;
                }
                if (not cap_can_take_reply(w))
                {
                    // The RECEIVER's table or its reply bound, not ours: no side effects yet.
                    return -KOS_EMFILE;
                }
                (void)wq_pop_highest(e->recv_waiters); // == w (nothing mutates under the lock)
                size_t n = send_len;
                if (w->ipc.len < n)
                {
                    n = w->ipc.len; // receiver-side request truncation
                }
                ep_copy(w->ipc.buf, buf, n);
                c->call_seq++; // new epoch BEFORE packing (the reply cap rides this seq)
                uint32_t rcap = KCAP_INVALID;
                // Not inside the assert: a compiled-out condition would drop the mint.
                int const minted = cap_install_reply(w, c, &rcap);
                KICKOS_ASSERT(minted == 0); // cap_can_take_reply probed w above
                write_recv_info(w->ipc.badge_out, KOS_BADGE_NONE, rcap);
                w->wait_result = static_cast<intptr_t>(n);
                // Repurpose our own ipc to the reply target (in-place buffer); the
                // request was fully copied above, so overwriting buf with the reply
                // is safe. Park queue-less bound to the reply cap, not the endpoint.
                c->ipc.buf = buf;
                c->ipc.len = recv_cap;
                c->ipc.badge_out = 0;
                c->call_rx_cap = recv_cap;
                c->call_state = CALL_REPLY_WAIT;
                // D1: donate to the server so the CPU goes straight to it.
                if (c->prio > w->prio)
                {
                    sched::set_prio(w, c->prio);
                }
                epoch = c->switch_count;
                sched::detach_current();
                c->state = ThreadState::BLOCKED;
                c->wait_queue = nullptr;
                reply_donor_park(w, c); // off the ready set: `link` is free to re-use
                sched::wake(w);
            }
            else
            {
                // Slowpath: no receiver parked. Park on send_waiters as a call; recv
                // completes the mint + transfer in server context later.
                c->ipc.buf = buf;
                c->ipc.len = send_len;
                c->ipc.badge_out = 0;
                c->call_rx_cap = recv_cap;
                c->call_seq++; // new epoch for this call (packed at pop time)
                c->call_state = CALL_SEND_WAIT;
                // D2: boost the conventional server NOW if this caller outranks it.
                if (e->server != nullptr and c->prio > e->server->prio)
                {
                    sched::set_prio(e->server, c->prio);
                }
                epoch = c->switch_count;
                wq_block(e->send_waiters);
            }
        }
        wq_confirm_resume(c, epoch);
        c->call_state = CALL_NONE; // B1: single-writer-clean on EVERY return path
        return static_cast<int32_t>(c->wait_result); // reply bytes, or -KOS_EPIPE/-KOS_EMFILE/-KOS_ENOSYS
    }

    // Copies the reply into the parked caller's buffer and wakes it. One-shot: the cap is
    // consumed on EVERY exit. Returns 0, or -KOS_E* (EBADF bad or non-reply cap, EFAULT bad
    // reply buffer, ESRCH the caller is gone or aborted).
    int endpoint_reply(uint32_t reply_cap, uintptr_t buf, size_t len)
    {
        if (len > KOS_EP_MSG_MAX)
        {
            len = KOS_EP_MSG_MAX; // the caller's capacity clamps it anyway
        }
        if (not user_readable_ok(buf, len))
        {
            return -KOS_EFAULT;
        }
        Thread* c = sched::current();
        if (c == nullptr)
        {
            return -KOS_EPERM; // no caller context (defensive)
        }
        IrqLock lock;
        CapEntry* e = cap_lookup(c, reply_cap);
        if (e == nullptr or e->type != static_cast<uint8_t>(CapType::CAP_REPLY))
        {
            return -KOS_EBADF; // bad / non-reply cap
        }
        Thread* caller = cap_reply_caller(*e); // full stale-resolve BEFORE consume
        // Consume the cap (one-shot): stale the handle + empty the slot, exactly once. The
        // release writes the free-list links over `obj`, so it must follow the resolve above.
        e->gen++;
        e->type = static_cast<uint8_t>(CapType::CAP_EMPTY);
        e->rights = 0;
        cap_run_free_release(c->caps, reply_cap & KCAP_INDEX_MASK, &c->cap_free_head);
        cap_reply_released(c);
        if (caller != nullptr)
        {
            // Must happen in the SAME step as the entry, and before either funnel
            // recompute below: a donor left linked past its cap is a boost the replier
            // can never shed.
            reply_donor_unpark(c, caller);
        }
        if (caller == nullptr)
        {
            // Cap consumed but no caller to complete: still revert our donation
            // through the funnel (defense-in-depth for when timed-call / kill land
            // and a stale reply becomes reachable). Unreachable today.
            sched::set_prio(c, thread_effective_prio(c));
            return -KOS_ESRCH; // caller aborted or reused; the cap is consumed regardless
        }
        size_t n = len;
        if (caller->call_rx_cap < n)
        {
            n = caller->call_rx_cap; // reply truncation into the caller's capacity
        }
        ep_copy(caller->ipc.buf, buf, n);
        caller->wait_result = static_cast<intptr_t>(n);
        caller->call_state = CALL_NONE;
        // D3: revert our donation through the single funnel (the cap is already gone
        // and the caller left REPLY_WAIT, so neither counts anymore).
        sched::set_prio(c, thread_effective_prio(c));
        sched::wake(caller); // D4: the caller >= us whenever it donated, so it runs now
        return 0;
    }
}
