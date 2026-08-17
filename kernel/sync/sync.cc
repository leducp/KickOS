// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/sync.h>
#include <kickos/sched.h>
#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/irqlock.h>

#include <kickos/sys/abi.h> // KOS_SEM_COUNT_MAX

#include <limits.h>

namespace kickos
{
    // FIFO among equal priority.
    Thread* wq_pop_highest(List& q)
    {
        Thread* best = wq_peek_highest(q);
        if (best != nullptr)
        {
            q.unlink(&best->link);
            best->clear_wait_edge();
            // No deadline cancel here. A pop is not an unpark: endpoint_recv pops a
            // CALL_SEND_WAIT caller straight into reply_donor_park, and cancelling here
            // would strip the deadline that must span both call phases. sched::wake owns
            // the cancel, because becoming ready is what ends a wait.
        }
        return best;
    }

    Thread* wq_peek_highest(List& q)
    {
        Thread* best = thread_of(q.head);
        if (best == nullptr)
        {
            return nullptr;
        }
        for (ListNode* n = q.head->next; n != nullptr; n = n->next)
        {
            Thread* t = thread_of(n);
            if (t->prio > best->prio)
            {
                best = t;
            }
        }
        return best;
    }

    // `epoch` MUST be c->switch_count sampled under the block lock immediately before
    // wq_block. On ARM the pended PendSV has not fired when that lock is released
    // (arch_irq_restore has no ISB), so the caller is still executing pre-switch and must
    // not trust anything a waker wrote. switch_to bumps the INCOMING thread's
    // switch_count, so an advance is proof of a real switch-in. Volatile, not atomic: the
    // value moves via the exception-mode switch, invisibly to this function, and it is 64
    // bits wide (sys/atomic.h refuses those). Zero iterations on the sim, where wq_block
    // switches synchronously.
    void wq_confirm_resume(Thread* c, uint64_t epoch)
    {
        // Reaching the cap means the switch is never coming (a masked or lost PendSV);
        // the pended switch otherwise fires as soon as the caller's IrqLock drops.
        uint32_t spin = 0;
        while (*static_cast<uint64_t volatile*>(&c->switch_count) == epoch)
        {
            if (++spin > KICKOS_POLL_SPIN_MAX)
            {
                kpanic(diag::kResumeNoSwitch);
            }
        }
    }

    // Returns when woken.
    void wq_block(List& q, WaitKind kind, void* obj)
    {
        Thread* c = sched::current();
        // BLOCKED before the detach: on_remove reads `state` to tell a park from a
        // set_prio re-seat, and only a park forfeits the RR slice remainder.
        c->state = ThreadState::BLOCKED;
        // Detach from the ready list FIRST: the ready list and the wait queues share the
        // TCB link node, so the push below would clobber the links the ready-list removal
        // still has to read.
        sched::detach_current();
        c->wait_queue = &q;
        c->wait_kind = kind;
        c->wait_obj = obj;
        q.push_back(&c->link);
        sched::reschedule();
    }

    void park_queueless(Thread* c, WaitKind kind, void* obj)
    {
        // Same ordering rule as wq_block: BLOCKED before the detach (on_remove reads it),
        // and the ready-list removal reads `link`, so it must run before anything re-uses
        // that node.
        c->state = ThreadState::BLOCKED;
        sched::detach_current();
        c->wait_queue = nullptr;
        c->wait_kind = kind;
        c->wait_obj = obj;
    }

    // The ceiling must be representable in Semaphore::count, else the bound in sem_post is
    // itself the overflow it prevents.
    static_assert(KOS_SEM_COUNT_MAX <= INT_MAX,
                  "KOS_SEM_COUNT_MAX must fit Semaphore::count (an int)");

    void sem_init(Semaphore* s, int initial)
    {
        s->count = initial;
        s->waiters = List{};
    }

    void sem_wait(Semaphore* s)
    {
        IrqLock lock;
        if (s->count > 0)
        {
            s->count--;
            return;
        }
        wq_block(s->waiters, WAIT_SEM, s);
        // The token was handed over directly by sem_post; there is nothing to decrement.
    }

    bool sem_trywait(Semaphore* s)
    {
        IrqLock lock;
        if (s->count > 0)
        {
            s->count--;
            return true;
        }
        return false;
    }

    bool sem_post(Semaphore* s)
    {
        IrqLock lock;
        Thread* w = wq_pop_highest(s->waiters);
        if (w != nullptr)
        {
            sched::wake(w); // the token goes straight to the waiter, count stays put
            return true;
        }
        if (s->count >= KOS_SEM_COUNT_MAX)
        {
            return false; // at the ceiling: refuse rather than wrap a signed int (UB)
        }
        s->count++;
        return true;
    }

    // Priority inheritance. sched::set_prio is the SOLE writer of an effective priority.
    // Inheritance does NOT propagate through semaphores: a thread blocked on a sem answers
    // nullptr from wait_mutex(), so the chain walk stops there.
    namespace
    {
        void held_push(Thread* owner, Mutex* m)
        {
            m->next_held = owner->held_list;
            owner->held_list = m;
        }

        void held_remove(Thread* owner, Mutex* m)
        {
            Mutex** pp = &owner->held_list;
            while (*pp != nullptr)
            {
                if (*pp == m)
                {
                    *pp = m->next_held;
                    m->next_held = nullptr;
                    return;
                }
                pp = &(*pp)->next_held;
            }
        }

        // Returns 0 when nobody is parked; 0 is below every real priority.
        uint8_t highest_waiter_prio(Mutex* m)
        {
            uint8_t best = 0;
            for (ListNode* n = m->waiters.head; n != nullptr; n = n->next)
            {
                Thread* t = thread_of(n);
                if (t->prio > best)
                {
                    best = t->prio;
                }
            }
            return best;
        }

        // Caller holds IrqLock, and must already have held_remove'd m from the releaser
        // and popped w off m->waiters; that pop is what cleared w's wait edge.
        // Both that clear and `status` belong to the WAKER, never to the woken thread
        // after it resumes: on ARM the woken thread resumes only after a deferred PendSV,
        // so a self-clear leaves a window in which a still-parked thread already answers
        // nullptr from wait_mutex() and the chain walk stops short of it, losing a boost
        // or a deadlock detection.
        void transfer_to(Mutex* m, Thread* w, intptr_t status)
        {
            m->owner = w;
            w->wait_result = status;
            held_push(w, m);
            // VACUOUS while the pop returns the highest-prio waiter: every waiter left on
            // m is then <= w. Kept as the general form so a change of pop policy does not
            // silently drop the new owner's inheritance.
            uint8_t wp = w->prio;
            uint8_t const hw = highest_waiter_prio(m);
            if (hw > wp)
            {
                wp = hw;
            }
            if (wp != w->prio)
            {
                sched::set_prio(w, wp);
            }
        }
    }

    // Unchecked preconditions: `caller` is already off the ready set and off every other
    // queue, so its `link` is free; and its CAP_REPLY is already installed in `server`'s
    // table. The entry is what the server closes and the membership is what the funnel
    // reads, so the two must move together or the funnel miscounts.
    void reply_donor_park(Thread* server, Thread* caller)
    {
        server->reply_waiters.push(&caller->link);
        // The server is the ONLY edge back from a queue-less reply park; endpoint_recv
        // re-parks a just-popped SEND_WAIT caller here, so this must run after that pop
        // cleared the endpoint edge.
        caller->wait_kind = WAIT_EP_REPLY;
        caller->wait_obj = server;
    }

    // Must run wherever a CAP_REPLY entry is consumed (reply, voluntary close, exit
    // teardown) and BEFORE the funnel recompute that follows, else the donor is counted
    // after its cap is gone. Membership is TESTED, not assumed: cap_reply_caller matches
    // only the low 8 bits of the call sequence, so a stale reply cap held by thread A can
    // resolve to a caller actually parked on thread B, and unlinking it from A would
    // splice B's list. The false return is what the caller must branch on: `link` is
    // ready-list XOR wait-queue XOR reply-donor, so completing a transaction against a
    // caller this did not unlink walks two lists through one node.
    bool reply_donor_unpark(Thread* server, Thread* caller)
    {
        // The clear rides the unlink: clearing on a miss would erase a live edge while the
        // caller stays linked on that other server's list.
        if (not server->reply_waiters.unlink_if_present(&caller->link))
        {
            return false;
        }
        caller->clear_wait_edge();
        return true;
    }

    void endpoint_server_clear(Endpoint* ep)
    {
        Thread* const prior = ep->server;
        if (prior == nullptr)
        {
            return;
        }
        int const index = kernel().endpoints.index_of(ep);
        KICKOS_ASSERT(index >= 0); // a biased -1 is the sentinel and would silently cut a chain
        uint16_t const self = ep_served_ref(index);
        uint16_t* pp = &prior->served_head;
        while (*pp != EP_SERVED_NONE)
        {
            if (*pp == self)
            {
                *pp = ep->next_served;
                ep->next_served = EP_SERVED_NONE;
                ep->server = nullptr;
                return;
            }
            pp = &kernel().endpoints.at(ep_served_index(*pp))->next_served;
        }
        KICKOS_ASSERT(false); // server set, yet absent from that server's chain
    }

    void endpoint_server_set(Endpoint* ep, Thread* t)
    {
        // Same thread recv'ing again: this early return keeps index_of's divide off the
        // per-message path.
        if (ep->server == t)
        {
            return;
        }
        endpoint_server_clear(ep);
        int const index = kernel().endpoints.index_of(ep);
        KICKOS_ASSERT(index >= 0);
        ep->next_served = t->served_head;
        t->served_head = ep_served_ref(index);
        ep->server = t;
    }

    // The single effective-priority funnel. Runs under IrqLock on every mutex unlock,
    // reply and close, so NO term added here may walk the capability table.
    uint8_t thread_effective_prio(Thread* t)
    {
        uint8_t p = t->base_prio;
        for (Mutex* h = t->held_list; h != nullptr; h = h->next_held)
        {
            uint8_t const hw = highest_waiter_prio(h);
            if (hw > p)
            {
                p = hw;
            }
        }
        for (ListNode* n = t->reply_waiters.head; n != nullptr; n = n->next)
        {
            Thread* caller = thread_of(n);
            if (caller->call_state == CALL_REPLY_WAIT and caller->prio > p)
            {
                p = caller->prio;
            }
        }
        // ep->server stays the authoritative "t is this endpoint's receiver" bit; the chain
        // only indexes it. Do not re-derive membership from t's capability table, and do not
        // sweep the endpoint pool either: at a large KICKOS_MAX_ENDPOINTS that sweep is the
        // masked window. A chain member is always a live slot: server is cleared before
        // recv_holders can reach 0, and the slot is freed only at zero refs.
        uint16_t r = t->served_head;
        while (r != EP_SERVED_NONE)
        {
            Endpoint* ep = kernel().endpoints.at(ep_served_index(r));
            KICKOS_DEBUG_ASSERT(ep->server == t);
            for (ListNode* n = ep->send_waiters.head; n != nullptr; n = n->next)
            {
                Thread* s = thread_of(n);
                if (s->call_state == CALL_SEND_WAIT and s->prio > p)
                {
                    p = s->prio;
                }
            }
            r = ep->next_served;
        }
        return p;
    }

    void mutex_init(Mutex* m)
    {
        m->owner = nullptr;
        m->waiters = List{};
        m->next_held = nullptr;
    }

    int mutex_lock(Mutex* m)
    {
        Thread* c = sched::current();
        uint64_t epoch = 0;
        {
            IrqLock lock;
            if (m->owner == nullptr)
            {
                m->owner = c;
                held_push(c, m);
                return 0;
            }
            if (m->owner == c)
            {
                return -KOS_EDEADLK; // a recursive lock is refused, never parked
            }
            // Pass 1: cycle detection, READ ONLY, so a refusal writes no boost. Walking
            // owner -> wait_mutex() -> owner reaching c means blocking c on m would close a
            // wait cycle. The depth bound only stops the walk on a PRE-EXISTING foreign
            // cycle; those threads are already deadlocked.
            {
                Thread* t = m->owner;
                int depth = 0;
                while (t != nullptr)
                {
                    if (t == c)
                    {
                        return -KOS_EDEADLK;
                    }
                    if (t->wait_mutex() == nullptr)
                    {
                        break; // t is runnable or parked on something else: chain ends
                    }
                    t = t->wait_mutex()->owner;
                    depth++;
                    if (depth > KICKOS_MAX_MUTEXES)
                    {
                        break;
                    }
                }
            }
            // Pass 2: the chain is now known acyclic. Stopping at the first owner already
            // at or above c's prio is sufficient; everything above it is too.
            {
                Thread* t = m->owner;
                int depth = 0;
                while (t != nullptr)
                {
                    if (t->prio >= c->prio)
                    {
                        break;
                    }
                    sched::set_prio(t, c->prio);
                    if (t->wait_mutex() == nullptr)
                    {
                        break;
                    }
                    t = t->wait_mutex()->owner;
                    depth++;
                    if (depth > KICKOS_MAX_MUTEXES)
                    {
                        break;
                    }
                }
            }
            // Must be sampled under the lock, immediately before parking; the barrier
            // below waits for it to advance.
            epoch = c->switch_count;
            // WAIT_MUTEX is what puts c on the chain walk above for the next blocker.
            // A waker transfers ownership and writes wait_result.
            wq_block(m->waiters, WAIT_MUTEX, m);
        }
        // The wait_result read must be BOTH outside the critical section AND after the
        // barrier: on ARM a couple of instructions retire on the not-yet-switched thread
        // after the unmask, so reading it earlier returns the pre-block value.
        wq_confirm_resume(c, epoch);
        return static_cast<int>(c->wait_result);
    }

    int mutex_unlock(Mutex* m)
    {
        IrqLock lock;
        Thread* c = sched::current();
        if (m->owner != c)
        {
            return -KOS_EPERM; // a non-owner unlock is a runtime error, never a panic
        }
        held_remove(c, m);
        Thread* w = wq_pop_highest(m->waiters);
        if (w == nullptr)
        {
            m->owner = nullptr;
            uint8_t const np = thread_effective_prio(c);
            if (np != c->prio)
            {
                // Lowering ourselves can make a middle-priority READY thread the highest
                // runnable, so the reschedule is not optional.
                sched::set_prio(c, np);
                sched::reschedule();
            }
            return 0;
        }
        transfer_to(m, w, 0);
        uint8_t const np = thread_effective_prio(c);
        if (np != c->prio)
        {
            sched::set_prio(c, np); // revert the boost over what we STILL hold
        }
        sched::wake(w);
        return 0;
    }

    void mutex_force_unlock(Mutex* m, Thread* dying)
    {
        // `dying` gets NO recompute: it only runs the rest of its own teardown, so it
        // stays boosted for the remainder of the sweep. That inversion is bounded and
        // unmasked because the sweep is chunked. The waiter is woken with
        // MUTEX_OWNER_DIED because the protected state may be inconsistent; it is never
        // stranded.
        held_remove(dying, m);
        Thread* w = wq_pop_highest(m->waiters);
        if (w == nullptr)
        {
            m->owner = nullptr;
            return;
        }
        transfer_to(m, w, MUTEX_OWNER_DIED);
        sched::wake(w);
    }
}
