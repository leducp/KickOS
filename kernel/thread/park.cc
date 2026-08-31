// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The park unwind MUST stay total over WaitKind: a cancel that reached only the kinds a
// thread happens to park on is not a kill.

#include <kickos/endpoint.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <kickos/sys/errno.h>

namespace kickos
{
    // Precondition: `t` is already off the timer delta list and its wait edge is intact; that
    // edge is the only thing naming the list `t` is on.
    void endpoint_wait_abort(Thread* t, intptr_t result)
    {
        switch (t->wait_kind)
        {
            case WAIT_EP_SEND:
            case WAIT_EP_RECV:
            {
                // Only a CALL_SEND_WAIT thread donates: a caller no receiver has taken yet,
                // which D2-boosted the endpoint's conventional server.
                bool const donor =
                    (t->wait_kind == WAIT_EP_SEND and t->call_state == CALL_SEND_WAIT);
                Endpoint* const e = t->wait_endpoint(); // before the edge is cleared
                KICKOS_ASSERT(e != nullptr);           // wq_block never parks here without one
                t->wait_queue->unlink(&t->link);
                t->clear_wait_edge();
                t->call_state = CALL_NONE;
                if (donor and e->server != nullptr)
                {
                    // Both the unlink and the CALL_NONE above MUST precede this: the funnel
                    // counts a SEND_WAIT donor still linked on send_waiters, so recomputing
                    // first would re-derive the very boost we are reverting.
                    sched::set_prio(e->server, thread_effective_prio(e->server));
                }
                break;
            }
            case WAIT_EP_REPLY:
            {
                // Queue-less: the server is the only edge back, and reply_donor_unpark
                // clears it as it unlinks.
                Thread* const server = t->wait_server();
                KICKOS_ASSERT(server != nullptr); // reply_donor_park never seats a null
                bool const unlinked = reply_donor_unpark(server, t);
                // The edge names the list this thread is on, so a miss is a kernel bug.
                KICKOS_ASSERT(unlinked);
                t->call_state = CALL_NONE;
                // The abandoned reply cap must die with the call it named: cap_reply_caller
                // matches only the low 8 bits of call_seq, so a cap left on this call's seq
                // resolves to this thread again after exactly 256 further calls.
                t->call_seq++;
                // D3, after the unlink and the CALL_NONE for the same reason as above.
                sched::set_prio(server, thread_effective_prio(server));
                // The server keeps the reply capability: reclaiming it would reach into
                // another thread's table. Its eventual reply consumes it and answers
                // -KOS_ESRCH.
                break;
            }
            default:
            {
                KICKOS_UNREACHABLE(::kickos::diag::kTimeoutNotEp);
            }
        }
        t->wait_result = result;
        sched::wake(t);
    }

    void thread_abort_park(Thread* t, intptr_t result)
    {
        switch (t->wait_kind)
        {
            case WAIT_EP_SEND:
            case WAIT_EP_RECV:
            case WAIT_EP_REPLY:
            {
                endpoint_wait_abort(t, result); // wakes it itself
                return;
            }
            case WAIT_MUTEX:
            {
                // Unlink first, so the funnel no longer counts this waiter when the owner's
                // inherited priority is recomputed. Only the immediate owner: a boost further
                // up a chain is reverted when that link is itself released.
                Mutex* const m = t->wait_mutex();
                KICKOS_ASSERT(m != nullptr); // WAIT_MUTEX never parks without one
                t->wait_queue->unlink(&t->link);
                t->clear_wait_edge();
                if (m->owner != nullptr)
                {
                    sched::set_prio(m->owner, thread_effective_prio(m->owner));
                }
                break;
            }
            case WAIT_SEM:
            case WAIT_IRQ:
            {
                // The count is untouched, so a later poster still hands its token to a
                // genuine waiter.
                t->wait_queue->unlink(&t->link);
                t->clear_wait_edge();
                break;
            }
            case WAIT_SLEEP:
            case WAIT_JOIN:
            case WAIT_LIVE_LAST:
            case WAIT_TASK_EMPTY:
            {
                // On no wait queue at all. A sleeper is on the timer delta list, and
                // sched::wake below is what takes it off.
                t->clear_wait_edge();
                break;
            }
            default:
            {
                KICKOS_UNREACHABLE(::kickos::diag::kParkNoKind);
            }
        }
        t->wait_result = result;
        sched::wake(t);
    }

    bool thread_cancel_escalate(Thread* t, uint8_t kind)
    {
        if (t == nullptr or t->state == ThreadState::EXITED
            or t->state == ThreadState::INACTIVE or t->dying)
        {
            return false;
        }
        // Escalation only, ordered NONE < KILL < SLAY: a kill arriving after a slay must not
        // hand back the cleanup window the slay took away.
        if (kind <= t->cancel_kind)
        {
            return false;
        }
        // Set before anything else: a thread that is READY right now must find it at its own
        // death point, and switch_to reads it when the scheduler next lists this thread.
        t->cancel_kind = kind;
        return true;
    }

    void thread_cancel_kind(Thread* t, uint8_t kind)
    {
        if (not thread_cancel_escalate(t, kind))
        {
            return;
        }
        if (t == sched::current())
        {
            return; // it is inside the kernel already and will see this on its way out
        }
        if (t->state != ThreadState::BLOCKED)
        {
            return;
        }
        // Reaching the death point needs it runnable, so the park ends here. A slain thread
        // never reads the result, but the unwind is the other side of each park's bookkeeping
        // and must still run.
        thread_abort_park(t, -KOS_ECANCELED);
    }

    void thread_cancel(Thread* t)
    {
        thread_cancel_kind(t, CANCEL_KILL);
    }

    void task_cancel_group(Task* t, uint8_t kind)
    {
        if (t == nullptr)
        {
            return;
        }
        Kernel& k = kernel();
        // thread_cancel_kind refuses a dying thread, so a task holding one thread matches
        // only itself.
        for (int s = 0; s < k.threads.next; s++)
        {
            Thread* const p = &k.threads.slots[s];
            if (p->task != t)
            {
                continue;
            }
            thread_cancel_kind(p, kind);
        }
    }
}
