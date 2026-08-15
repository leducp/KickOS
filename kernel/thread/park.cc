// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Breaking a park from OUTSIDE the parked thread: a deadline that expired, or a
// cancellation. Both hand the sleeper a result it did not wait for and neither grants it
// whatever it was parked on, so both need the same per-WaitKind unwind: which list to
// unlink and which priority donation to revert. That is why they live together.
//
// The park unwind is TOTAL over WaitKind. A cancel that reached only the kinds a thread
// happens to park on would not be a kill (docs/design-task-layer.md open question 1).

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
    // Unwind `t` out of whichever endpoint park it sits under, with `t` already off the timer
    // delta list if it was on one and its wait edge still intact: that edge is the only thing
    // naming the list `t` is on. This function exists so its callers decide only THAT the park
    // must end; every endpoint internal below belongs to this layer.
    void endpoint_wait_abort(Thread* t, intptr_t result)
    {
        switch (t->wait_kind)
        {
            case WAIT_EP_SEND:
            case WAIT_EP_RECV:
            {
                // Only a CALL_SEND_WAIT thread donates: it is a caller no receiver has
                // taken yet, and it D2-boosted the endpoint's conventional server. A plain
                // sender and a receiver are CALL_NONE.
                bool const donor =
                    (t->wait_kind == WAIT_EP_SEND and t->call_state == CALL_SEND_WAIT);
                Endpoint* const e = t->wait_endpoint(); // before the edge is cleared
                KICKOS_ASSERT(e != nullptr);           // wq_block never parks here without one
                t->wait_queue->unlink(&t->link);
                t->clear_wait_edge();
                t->call_state = CALL_NONE;
                // An endpoint with no conventional receiver yet has nobody to have boosted.
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
                // Unlike the userspace-reachable stale-cap route into reply/close, the edge
                // here NAMES the list this thread is on, so a miss is a kernel bug.
                KICKOS_ASSERT(unlinked);
                t->call_state = CALL_NONE;
                // The abandoned reply cap must die with the call it named: cap_reply_caller
                // matches only the low 8 bits of call_seq, so a cap left on this call's seq
                // resolves to this thread again after exactly 256 further calls.
                t->call_seq++;
                // D3, after the unlink and the CALL_NONE for the same reason as above.
                sched::set_prio(server, thread_effective_prio(server));
                // The server KEEPS the reply capability. Reclaiming an entry from another
                // thread's table would reach across a containment boundary; the residue is
                // bounded by KICKOS_CAP_REPLY_MAX against Thread::cap_reply_live, and the
                // server's eventual reply consumes it and answers -KOS_ESRCH.
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
                // Ownership is NOT transferred: this thread stops waiting for the mutex, it
                // does not acquire it. Unlink first, so the funnel no longer counts this
                // waiter when the owner's inherited priority is recomputed. Only the
                // IMMEDIATE owner is recomputed, as mutex_unlock does: a boost further up a
                // chain is reverted when that link is itself released.
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
                // Both are parked on a real wait queue and the edge names it. No token is
                // consumed and no count moves: the semaphore is left exactly as it was, so a
                // later poster still hands its token to a genuine waiter.
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

    void thread_cancel_kind(Thread* t, uint8_t kind)
    {
        if (t == nullptr or t->state == ThreadState::EXITED
            or t->state == ThreadState::INACTIVE or t->dying)
        {
            return;
        }
        // ESCALATION ONLY, and it reads as a comparison because the enum's values are
        // ordered NONE < KILL < SLAY. A kill arriving after a slay must not hand back the
        // cleanup window the slay took away, and a kind at or below the recorded one has
        // nothing to add, so the park below is not broken a second time either.
        if (kind <= t->cancel_kind)
        {
            return;
        }
        // Set before anything else: the thread's own death point reads it, and a thread that
        // is READY right now must find it there rather than be woken for it. A CANCEL_SLAY
        // written here is what switch_to reads when the scheduler next lists this thread.
        t->cancel_kind = kind;
        if (t == sched::current())
        {
            return; // it is inside the kernel already and will see this on its way out
        }
        if (t->state != ThreadState::BLOCKED)
        {
            return;
        }
        // Reaching the death point needs it RUNNABLE, so the park ends here. The result is
        // what a primitive with an error channel reports; one without (sem_wait, sleep)
        // simply returns, and the death point is what stops it going round again. A SLAIN
        // thread never reads it, its resume being claimed before it returns to userspace,
        // but the unwind the abort performs is the OTHER side of each park's bookkeeping and
        // must still run, so this is not a branch to skip.
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
        // Bounded by the pool cursor, the same scan exit_current already makes to find its
        // joiners. A task holding one thread matches only itself, and thread_cancel_kind
        // refuses a dying one, which is what makes the implicit grouping cost a comparison
        // and nothing else.
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
