// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The IPC fastpath's shared leaf: everything the register-carrying kos_call does that is
// not frame surgery. The arch enters it from inside the syscall trap, BEFORE the exception
// return that would otherwise land in the privileged thread-mode trampoline and run the
// generic dispatch (docs/design-m5-ipc-fastpath.md section 4.8).
//
// Two rules govern every line below.
//
// It decides BEFORE it mutates. A refusal must leave the trap able to continue into the
// generic dispatch with nothing committed, so every test that can fail runs before the
// first write, which is why the reply-slot probe is a step of its own rather than part of
// the mint it guards.
//
// It reimplements no refusal. A refusal is a fall-through, never an errno: the caller
// re-issues through KOS_SYS_CALL and the buffer-carrying endpoint_call produces the answer.

#include <kickos/bench.h>
#include <kickos/cap.h>
#include <kickos/endpoint.h>
#include <kickos/instance.h>
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
        // Bumped at the commit point below. Single-writer under the trap's own interrupt
        // mask, so a plain type. Defined OUTSIDE the arch guard, so a backend with no
        // fastpath reads 0 rather than failing to link.
        uint32_t g_ipc_fast_taken = 0;
    }

    uint32_t ipc_fast_taken_count()
    {
        return g_ipc_fast_taken;
    }
}

#if KICKOS_ARCH_HAS_IPC_FASTPATH

namespace kickos
{
    namespace
    {
        constexpr uint32_t KOS_BADGE_NONE = 0;
    }

    // The arch prologues branch on the syscall number as a LITERAL, because an asm trap
    // handler cannot include an enum. This is what makes a renumbering a build error
    // instead of a fastpath that silently stops being taken.
    static_assert(KOS_SYS_CALL_REG == 56, "the arch prologues branch on the literal 56");

    // `args` is the caller's saved argument registers, contiguous and writable:
    //   in   args[1] = endpoint cap, args[2] = packed lengths, args[3..] = request
    //   out  args[1..] = reply payload; args[0] is the result, written by the switch
    //        that resumes this thread (Thread::call_frame_parked)
    // The request and the reply overlap deliberately: both are bounded by
    // KOS_CALL_REG_WORDS and the request is fully consumed before the caller parks.
    //
    // Returns the incoming thread's context for the arch to restore, or nullptr for a
    // refusal, in which case NOTHING has been written.
    extern "C" struct arch_context* kickos_ipc_fastpath(uint32_t* args)
    {
        // Entered with interrupts masked by the trap itself, so there is no IrqLock here
        // and no lock release to order anything against.
        Thread* c = kernel().current;
        if (c == nullptr)
        {
            return nullptr;
        }
        // The generic dispatch tests this on entry and exits the thread. Bypassing dispatch
        // must not bypass that, and a cancelled caller is rare enough to hand back.
        if (c->cancel_kind != CANCEL_NONE or c->dying)
        {
            return nullptr;
        }
        size_t const send_len = kos_call_lens_send(args[2]);
        size_t const recv_cap = kos_call_lens_recv(args[2]);
        if (send_len > (size_t)KOS_CALL_REG_BYTES or recv_cap > (size_t)KOS_CALL_REG_BYTES)
        {
            return nullptr; // oversize payload: it does not fit the registers it arrived in
        }
        int err = 0;
        Endpoint* e = static_cast<Endpoint*>(
            cap_resolve_e(c, args[1], CapType::CAP_ENDPOINT, CAP_SIGNAL, &err));
        if (e == nullptr or e->recv_holders == 0)
        {
            return nullptr; // bad cap, no SIGNAL right, or a dead endpoint
        }
        Thread* w = wq_peek_highest(e->recv_waiters);
        if (w == nullptr)
        {
            return nullptr; // no parked receiver: there is nothing to hand off to
        }
        if (w->ipc.badge_out == 0 or w->dying)
        {
            return nullptr; // info-less receiver cannot host a call; a dying one takes no cap
        }
        if (c->prio > w->prio)
        {
            // Donation applicable. REFUSED in v1 and structured so it could be admitted:
            // the test is here, before the first mutation, so admitting it is a
            // sched::set_prio beside the park below rather than a re-ordering.
            // docs/design-m5-ipc-fastpath.md section 4.9 states what this concedes.
            return nullptr;
        }
        if (not cap_can_take_reply(w))
        {
            return nullptr; // the RECEIVER's table or its reply bound, and no side effect yet
        }

        // Every refusal is behind us. From here the call completes.
        g_ipc_fast_taken++;
        (void)wq_pop_highest(e->recv_waiters); // == w, nothing mutates under this mask
        size_t n = send_len;
        if (w->ipc.len < n)
        {
            n = w->ipc.len; // receiver-side request truncation
        }
        // The request end is the caller's SAVED TRAP FRAME and so kernel storage, which
        // is what a null owner names; the receiver's end is its own space.
        ep_copy(ipc_buf_space(w), w->ipc.buf, nullptr, reinterpret_cast<uintptr_t>(&args[3]),
                n);
        c->call_seq++; // new epoch BEFORE the mint: the reply cap rides this seq
        uint32_t rcap = KCAP_INVALID;
        int const minted = cap_install_reply(w, c, &rcap);
        KICKOS_ASSERT(minted == 0); // cap_can_take_reply probed w above
        (void)minted;
        write_recv_info(user_space_of(w), w->ipc.badge_out, KOS_BADGE_NONE, rcap);
        w->wait_result = static_cast<intptr_t>(n);

        // The reply target is the caller's OWN saved frame, so endpoint_reply's ep_copy
        // lands the payload straight in the registers the restore will pop. That is what
        // makes the register form a round trip rather than half of one, and it is why this
        // path needs no user buffer and no bound check on one. call_frame_parked is what
        // tells endpoint_reply that this ipc.buf is kernel storage and not a user address
        // in the caller's space, so it is set below rather than at the park.
        c->ipc.buf = reinterpret_cast<uintptr_t>(&args[1]);
        c->ipc.len = recv_cap;
        c->ipc.badge_out = 0;
        c->call_rx_cap = recv_cap;
        c->call_state = CALL_REPLY_WAIT;
        // No kernel continuation exists for this park: the caller's resume is the arch
        // restoring the frame this call trapped on. So the result cannot be returned, and
        // the switch that resumes it stores it into args[0] instead.
        c->call_frame_parked = 1;
        park_queueless(c, WAIT_EP_REPLY, w);
        reply_donor_park(w, c);
        // No deadline: the untimed kos_call is the only shape the stub selects this number
        // for, and a timed one keeps the buffer form.

        if (not sched::wake_no_resched(w))
        {
            // Refusing here would strand a popped receiver, so it cannot be a refusal. It
            // is also not reachable: w was parked on recv_waiters and this is the pop.
            KICKOS_ASSERT(false);
        }
        Thread* next = kernel().policy->pick_next();
        KICKOS_ASSERT(next != nullptr);
        KICKOS_ASSERT(next != c); // c just parked, so the policy cannot list it
        return sched::switch_prepare(next);
    }
}

#endif
