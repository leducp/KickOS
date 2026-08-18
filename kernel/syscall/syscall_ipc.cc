// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Synchronous IPC: endpoint create plus the send/recv/call/reply rendezvous. send, recv
// and call MUST be entered with no caller-held IrqLock: each releases its own before the
// resume barrier, and a spanning caller lock keeps BASEPRI raised across
// wq_confirm_resume and livelocks ARM.

#include <kickos/bench.h>
#include <kickos/cap.h>
#include <kickos/endpoint.h>
#include <kickos/instance.h>
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
        // The badge every message carries; there is no per-endpoint badge scheme.
        // Distinct from ipc.badge_out == 0, which means the receiver asked for no badge.
        constexpr uint32_t KOS_BADGE_NONE = 0;

        void park_deadline_arm(Thread* t, uint32_t timeout_us)
        {
            if (timeout_us != KOS_TIMEOUT_NONE)
            {
                ktime_deadline_arm(t, timeout_us);
            }
        }
    }

    // --- Endpoint capability (IPC rendezvous; mirrors sem_create) ---------------
    // A thread names an endpoint by a per-thread CAP_ENDPOINT capability; the creator cap
    // carries WAIT|SIGNAL|TRANSFER, send needs CAP_SIGNAL and recv needs CAP_WAIT. Two
    // counters track it, endpoint_refs (all caps) and recv_holders (WAIT-bearing caps); a
    // rollback must unwind BOTH.
    int endpoint_create(uint32_t* out_cap)
    {
        IrqLock lock;
        *out_cap = KCAP_INVALID;
        Thread* c = sched::current();
        if (c == nullptr)
        {
            return -KOS_EPERM;
        }
        int const i = kernel().endpoints.alloc();
        if (i < 0)
        {
            return -KOS_ENOMEM;
        }
        Endpoint* ep = kernel().endpoints.at(i);
        ep->send_waiters = List{};
        ep->recv_waiters = List{};
        ep->recv_holders = 1;         // creator holds a WAIT-bearing cap
        ep->server = nullptr;         // no conventional receiver until the first recv
        // A reused slot keeps its last occupant's link, and "server null implies unlinked"
        // must hold from the moment the slot is handed out.
        ep->next_served = EP_SERVED_NONE;
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

    // Synchronous send: rendezvous with a parked receiver (deliver now) or park. Entered
    // with no caller-held IrqLock, per the file header.
    // Returns bytes transferred (>= 0), or -KOS_E* (EINVAL oversize, EFAULT bad buffer,
    // EBADF/EPERM bad cap or missing SIGNAL, EPIPE dead endpoint / last receiver left,
    // ETIMEDOUT `timeout_us` elapsed, ECANCELED cancelled while parked; in the last three
    // cases nothing was sent).
    // `timeout_us` is relative microseconds and bounds the PARK only; KOS_TIMEOUT_NONE
    // parks forever.
    int32_t endpoint_send(uint32_t cap, uintptr_t buf, size_t len, uint32_t timeout_us)
    {
        if (len > KOS_EP_MSG_MAX)
        {
            return -KOS_EINVAL; // F4: oversize is rejected, never clamped
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
                    n = w->ipc.len; // datagram truncation, not an error
                }
                ep_copy(w->ipc.buf, buf, n);
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
            park_deadline_arm(c, timeout_us);
            wq_block(e->send_waiters, WAIT_EP_SEND, e);
        }
        wq_confirm_resume(c, epoch);
        // n (>= 0), -KOS_EPIPE (last receiver left), -KOS_ETIMEDOUT (ktime_on_timer), or
        // -KOS_ECANCELED (this parked caller was cancelled)
        return static_cast<int32_t>(c->wait_result);
    }

    // See cap.h. endpoint_send's parked-receiver arm and NOTHING else: it never parks, a
    // fault record having to reach neither a driver that is mid-write nor one already gone.
    // Resolves through the KERNEL's own identity ref on the published endpoint
    // (cap_console_publish), not through any cap table, which a thread spawned before the
    // publish never had seated. `buf` is kernel memory, so there is no user_readable_ok.
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
        kaccess_to_user(w->ipc.buf, buf, n);
        write_recv_info(w->ipc.badge_out, KOS_BADGE_NONE, KCAP_INVALID);
        w->wait_result = static_cast<intptr_t>(n);
        sched::wake(w);
        return static_cast<int32_t>(n);
    }

    // Synchronous recv: take from a parked sender (copy now) or park. Entered with no
    // caller-held IrqLock, per the file header.
    // Returns bytes received (>= 0), or -KOS_E* (EFAULT bad buffer/out-ptr, EINVAL
    // misaligned badge, EBADF/EPERM bad cap or missing WAIT, ETIMEDOUT the deadline elapsed
    // with no sender, ECANCELED cancelled while parked). n == 0 is a VALID zero-length
    // signal, not an error.
    // Under `timed`, `badge_out` names a kos_recv_timed_opts and not a bare kos_recv_info:
    // the deadline is read out of it here and everything downstream sees only the nested
    // kos_recv_info.
    int32_t endpoint_recv(uint32_t cap, uintptr_t buf, size_t cap_len, uintptr_t badge_out,
                          bool timed)
    {
        if (cap_len > KOS_EP_MSG_MAX)
        {
            cap_len = KOS_EP_MSG_MAX; // capacity clamp is harmless
        }
        if (not user_writable_ok(buf, cap_len))
        {
            return -KOS_EFAULT;
        }
        // REWRITES badge_out to the kos_recv_info nested in the opts struct. Each arm below
        // validates the out-ptr on the address the kernel actually stores to.
        uint32_t timeout_us = KOS_TIMEOUT_NONE;
        if (timed)
        {
            // The deadline rides the opts struct: without it there is nothing to read.
            if (badge_out == 0)
            {
                return -KOS_EINVAL;
            }
            if ((badge_out & (alignof(uint32_t) - 1)) != 0)
            {
                return -KOS_EINVAL; // load-bearing for the privileged read below
            }
            // IN-OUT here, so readable as well as writable; plain recv reads nothing.
            if (not user_readable_ok(badge_out, sizeof(kos_recv_timed_opts))
                or not user_writable_ok(badge_out, sizeof(kos_recv_timed_opts)))
            {
                return -KOS_EFAULT;
            }
            // Copied ONCE, before anything can park: the struct stays user-writable, so a
            // re-read after the park would see whatever the caller has since put there.
            kaccess_from_user(&timeout_us,
                              badge_out + offsetof(kos_recv_timed_opts, timeout_us),
                              sizeof(timeout_us));
            badge_out = badge_out + offsetof(kos_recv_timed_opts, info);
        }
        else // binds to the `if` below, past the comment block
        // The out-ptr delivers a kos_recv_info (8 bytes, 4-aligned), not a bare badge u32.
        // badge_out == 0 means an info-less recv, which cannot host a call. NOT reached on
        // the timed path: the rewritten badge_out was already proved writable and 4-aligned
        // inside the opts struct.
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
            return -KOS_EPERM;
        }
        uint32_t epoch = 0;
        {
            IrqLock lock;
            KICKOS_BENCH_MARK(bm_rlocked);
            KICKOS_BENCH_MARK(bm_rresolve);
            int err = 0;
            Endpoint* e = static_cast<Endpoint*>(
                cap_resolve_e(c, cap, CapType::CAP_ENDPOINT, CAP_WAIT, &err));
            if (e == nullptr)
            {
                return -err; // EBADF (bad cap) or EPERM (no WAIT right)
            }
            endpoint_server_set(e, c); // the conventional receiver (D2 boost target)
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
                        sched::wake(s);
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
                    KICKOS_ASSERT(minted == 0);
                    write_recv_info(badge_out, KOS_BADGE_NONE, rcap);
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
                ep_copy(buf, s->ipc.buf, n);
                write_recv_info(badge_out, KOS_BADGE_NONE, KCAP_INVALID);
                s->wait_result = static_cast<intptr_t>(n);
                sched::wake(s);
                return static_cast<int>(n);
            }
            KICKOS_BENCH_SPAN(PH_RECV_SCAN, bm_rscan);
            KICKOS_BENCH_MARK(bm_rpark);
            c->ipc.buf = buf;
            c->ipc.len = cap_len;
            c->ipc.badge_out = badge_out;
            epoch = c->switch_count;
            park_deadline_arm(c, timeout_us);
            wq_block(e->recv_waiters, WAIT_EP_RECV, e);
            KICKOS_BENCH_SPAN(PH_RECV_PARK, bm_rpark);
            KICKOS_BENCH_SPAN(PH_RECV_LOCKED, bm_rlocked);
        }
        wq_confirm_resume(c, epoch);
        return static_cast<int32_t>(c->wait_result);
    }

    // Synchronous call: deliver a request to the endpoint's server, park until it replies.
    // In-place buffer (request out, reply back). Entered with no caller-held IrqLock, per
    // the file header.
    // Returns reply bytes (>= 0), or -KOS_E* (EINVAL oversize, EFAULT bad buffer,
    // EBADF/EPERM bad cap or no SIGNAL, EPIPE dead endpoint or server died, EMFILE the
    // SERVER's table is full or it is at its inbound reply bound, ENOSYS server took an
    // info-less recv, ETIMEDOUT `timeout_us` elapsed, ECANCELED cancelled while parked on
    // either half of the call).
    // `timeout_us` bounds the WHOLE call, not one park: ONE deadline covers the wait on
    // send_waiters AND the wait for the reply. KOS_TIMEOUT_NONE parks forever on either
    // path.
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
        if (not user_readable_ok(buf, send_len) or not user_writable_ok(buf, recv_cap))
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
            if (e->recv_holders == 0)
            {
                return -KOS_EPIPE; // dead endpoint: no receiver can ever exist
            }
            KICKOS_BENCH_MARK(bm_peek);
            Thread* w = wq_peek_highest(e->recv_waiters);
            KICKOS_BENCH_SPAN(PH_CALL_PEEK, bm_peek);
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
                ep_copy(w->ipc.buf, buf, n);
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
                write_recv_info(w->ipc.badge_out, KOS_BADGE_NONE, rcap);
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
                // trip. Fastpath-only, too: a span shared with the slowpath arm would report
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

    // Copies the reply into the parked caller's buffer and wakes it. One-shot: the cap is
    // consumed on EVERY exit. Returns 0, or -KOS_E* (EBADF bad or non-reply cap, EFAULT bad
    // reply buffer, ESRCH the caller is gone or aborted).
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
        KICKOS_BENCH_MARK(bm_locked);
        KICKOS_BENCH_MARK(bm_lookup);
        CapEntry* e = cap_lookup(c, reply_cap);
        if (e == nullptr or e->type != static_cast<uint8_t>(CapType::CAP_REPLY))
        {
            return -KOS_EBADF;
        }
        Thread* caller = cap_reply_caller(*e); // full stale-resolve BEFORE consume
        // cap_run_free_release writes the free-list links over `obj`, so it must follow the
        // resolve above.
        e->gen++;
        e->type = static_cast<uint8_t>(CapType::CAP_EMPTY);
        e->rights = 0;
        cap_run_free_release(c->caps, reply_cap & KCAP_INDEX_MASK, &c->cap_free_head);
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
        ep_copy(caller->ipc.buf, buf, n);
        KICKOS_BENCH_SPAN(PH_REPLY_COPY, bm_copy);
        caller->wait_result = static_cast<intptr_t>(n);
        caller->call_state = CALL_NONE;
        KICKOS_BENCH_MARK(bm_funnel);
        // D3: revert our donation through the single funnel; the cap is gone and the caller
        // left REPLY_WAIT, so neither counts anymore.
        sched::set_prio(c, thread_effective_prio(c));
        KICKOS_BENCH_SPAN(PH_REPLY_FUNNEL, bm_funnel);
        KICKOS_BENCH_MARK(bm_wake);
        sched::wake(caller); // D4: the caller >= us whenever it donated, so it runs now
        KICKOS_BENCH_SPAN(PH_REPLY_WAKE, bm_wake);
        // Both close before the return, where `lock` releases and a pended switch fires.
        KICKOS_BENCH_SPAN(PH_REPLY_LOCKED, bm_locked);
        KICKOS_BENCH_SPAN(PH_REPLY_TOTAL, bm_total);
        return 0;
    }
}
