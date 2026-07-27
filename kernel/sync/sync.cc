// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/sync.h>
#include <kickos/sched.h>
#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/irqlock.h>

#include <kickos/sys/abi.h> // KOS_SEM_COUNT_MAX (the ceiling is ABI, not policy)

#include <limits.h>

namespace kickos
{
    // Remove and return the highest-priority waiter (FIFO among equal priority).
    // The list mechanics are shared (List); the priority scan is the only
    // wait-queue-specific policy on top.
    Thread* wq_pop_highest(List& q)
    {
        Thread* best = wq_peek_highest(q);
        if (best != nullptr)
        {
            q.unlink(&best->link); // O(1): List is doubly-linked, no predecessor scan
            best->wait_queue = nullptr;
        }
        return best;
    }

    // Same scan as wq_pop_highest but leaves the queue intact (B3 probe-before-pop).
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

    // Resume barrier for the wake protocol (see sync.h). `epoch` is c->switch_count
    // sampled under the block lock immediately before wq_block. After the block
    // scope's lock is released, ARM's pended PendSV has not fired yet (arch_irq_restore
    // has no ISB), so this thread is still executing pre-switch; spin until it is
    // genuinely switched back in (switch_count advances -- switch_to bumps the INCOMING
    // thread's count). Volatile so the compiler reloads each iteration; the value moves
    // via the exception-mode switch, invisible to this function. Zero iterations on the
    // sim (the switch already happened synchronously inside wq_block).
    void wq_confirm_resume(Thread* c, uint64_t epoch)
    {
        // Bounded like every other synchronous poll in the tree (KICKOS_POLL_SPIN_MAX;
        // the console-publish drain a few files away is the pattern). This one used to
        // be the exception -- a masked or lost PendSV left it spinning forever with no
        // diagnostic, which is the one failure mode this project refuses everywhere
        // else. The cap is enormous relative to a real wait (the switch is pended and
        // fires as soon as the caller's IrqLock drops), so reaching it means the switch
        // is never coming: fail LOUD.
        uint32_t spin = 0;
        while (*static_cast<uint64_t volatile*>(&c->switch_count) == epoch)
        {
            if (++spin > KICKOS_POLL_SPIN_MAX)
            {
                kpanic("wq_confirm_resume: switch never landed");
            }
        }
    }

    // Park the current thread on `q` and switch away; returns when woken.
    void wq_block(List& q)
    {
        Thread* c = sched::current();
        // Detach from the ready list FIRST: the ready list and wait queues
        // share the TCB link node, so pushing onto the wait queue would clobber
        // the links that the ready-list removal needs to read.
        sched::detach_current();
        c->state = ThreadState::BLOCKED;
        c->wait_queue = &q;
        q.push_back(&c->link);
        sched::reschedule(); // switch away; returns when woken
    }

    // --- Semaphore -------------------------------------------------------------
    // The ceiling must itself be representable in Semaphore::count, else the bound in
    // sem_post is the overflow it exists to prevent.
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
        wq_block(s->waiters);
        // Woken with the token handed to us directly; nothing to decrement.
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
            sched::wake(w); // token handed directly to the woken waiter
            return true;
        }
        if (s->count >= KOS_SEM_COUNT_MAX)
        {
            return false; // at the ceiling: refuse rather than wrap a signed int (UB)
        }
        s->count++;
        return true;
    }

    // --- Mutex (priority inheritance) -----------------------------------------
    // Plain PI, boost-on-contention, revert-by-recompute over the owner's held list.
    // All effective-priority writes funnel through sched::set_prio (the sole writer);
    // wq_pop_highest needs no change (it rescans lazily, so a boosted parked waiter is
    // never re-queued). Inheritance does NOT propagate through semaphores: a thread
    // blocked on a sem has blocked_on == nullptr, so the chain walk stops there.
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

        // Highest priority among the threads parked on m, or 0 (below any real prio)
        // if none. Same scan shape as wq_pop_highest.
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

        // Hand ownership of m to the popped waiter w. `status` and the blocked_on
        // clear are written by the WAKER here, under the lock -- NOT by the woken
        // thread after it resumes. On ARM the woken thread resumes only after a
        // deferred PendSV, so a self-clear of blocked_on would leave a window where a
        // parked thread has blocked_on == nullptr and the chain walk stops short of
        // it (missed boost / missed deadlock). Caller holds IrqLock and has already
        // held_remove'd m from the releaser and popped w off m->waiters.
        void transfer_to(Mutex* m, Thread* w, intptr_t status)
        {
            m->owner = w;
            w->wait_result = status;
            w->blocked_on = nullptr;
            held_push(w, m);
            // Boost w from the remaining waiters. This is VACUOUS at transfer time --
            // wq_pop_highest returned the highest-prio waiter, so every waiter still on
            // m is <= w's effective prio -- but kept as the correct general form (H4:
            // the new owner inherits the remaining waiters) so it stays right if the
            // pop policy ever changes.
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

    // The single effective-priority funnel (D3). Mutex term (the old recompute_prio),
    // then the call/reply donors: each live CAP_REPLY in t's table names a parked caller,
    // and each endpoint where t is the server contributes its highest parked SEND_WAIT
    // caller. Bounded by KICKOS_MAX_HANDLES; no object-pool scan.
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
        for (int i = 0; i < KICKOS_MAX_HANDLES; i++)
        {
            CapEntry const& e = t->handles[i];
            if (e.type == static_cast<uint8_t>(CapType::CAP_REPLY))
            {
                Thread* caller = cap_reply_caller(e.obj);
                if (caller != nullptr and caller->prio > p)
                {
                    p = caller->prio;
                }
            }
            else if (e.type == static_cast<uint8_t>(CapType::CAP_ENDPOINT)
                     and (e.rights & CAP_WAIT) != 0)
            {
                Endpoint* ep = kernel().endpoints.resolve(e.obj);
                if (ep != nullptr and ep->server == t)
                {
                    for (ListNode* n = ep->send_waiters.head; n != nullptr; n = n->next)
                    {
                        Thread* s = thread_of(n);
                        if (s->call_state == CALL_SEND_WAIT and s->prio > p)
                        {
                            p = s->prio;
                        }
                    }
                }
            }
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
                // Fast path: two stores, no prio work.
                m->owner = c;
                held_push(c, m);
                return 0;
            }
            if (m->owner == c)
            {
                return -KOS_EDEADLK; // self-deadlock: recursive lock is refused, not parked
            }
            // Pass 1: cycle/deadlock detection, READ ONLY. Walk owner -> blocked_on ->
            // owner ... ; if it reaches c, blocking c on m would close a wait cycle.
            // The depth bound stops a PRE-EXISTING foreign cycle (those threads are
            // already deadlocked; the bound just prevents an unbounded walk).
            {
                Thread* t = m->owner;
                int depth = 0;
                while (t != nullptr)
                {
                    if (t == c)
                    {
                        return -KOS_EDEADLK; // would deadlock -- refuse, no boost written
                    }
                    if (t->blocked_on == nullptr)
                    {
                        break; // t is runnable or sem-blocked: chain ends
                    }
                    t = t->blocked_on->owner;
                    depth++;
                    if (depth > KICKOS_MAX_MUTEXES)
                    {
                        break;
                    }
                }
            }
            // Pass 2: boost the chain (proven acyclic). Raise each owner to c's prio
            // until one is already at/above it (I2 holds inductively above that point).
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
                    if (t->blocked_on == nullptr)
                    {
                        break;
                    }
                    t = t->blocked_on->owner;
                    depth++;
                    if (depth > KICKOS_MAX_MUTEXES)
                    {
                        break;
                    }
                }
            }
            c->blocked_on = m;
            // Snapshot the switch-in epoch under the lock, right before parking: the
            // resume barrier below waits for it to advance (see wq_confirm_resume).
            epoch = c->switch_count;
            wq_block(m->waiters); // parks; a waker transfers ownership + writes wait_result
        }
        // Resume barrier. The wait_result read MUST be both outside the critical
        // section AND confirmed post-resume. On ARM arch_switch only PENDS PendSV and
        // arch_irq_restore has no ISB, so 1-2 instructions retire on the STILL-current
        // (not-yet-switched) thread after the unmask -- reading wait_result here
        // without confirmation would return the pre-block value. wq_confirm_resume
        // spins until this thread is genuinely switched back in (its epoch advances);
        // the waker already cleared blocked_on and wrote wait_result under the lock.
        wq_confirm_resume(c, epoch);
        return static_cast<int>(c->wait_result);
    }

    int mutex_unlock(Mutex* m)
    {
        IrqLock lock;
        Thread* c = sched::current();
        if (m->owner != c)
        {
            return -KOS_EPERM; // only the owner may unlock -- runtime error, never a panic
        }
        held_remove(c, m);
        Thread* w = wq_pop_highest(m->waiters);
        if (w == nullptr)
        {
            m->owner = nullptr;
            uint8_t const np = thread_effective_prio(c);
            if (np != c->prio)
            {
                // Lowering ourselves may make a middle-priority READY thread the
                // highest runnable -- reschedule to give it the CPU (H8).
                sched::set_prio(c, np);
                sched::reschedule();
            }
            return 0;
        }
        transfer_to(m, w, 0); // hand off + boost w from remaining waiters
        uint8_t const np = thread_effective_prio(c);
        if (np != c->prio)
        {
            sched::set_prio(c, np); // revert our boost over what we STILL hold
        }
        sched::wake(w); // reschedule inside handles preemption by the new owner
        return 0;
    }

    void mutex_force_unlock(Mutex* m, Thread* dying)
    {
        // R3: the owner is EXITED, so it is never scheduled again -- skip ITS
        // recompute. Deliver MUTEX_OWNER_DIED to the woken waiter (the protected
        // state may be inconsistent). Never strands a waiter.
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
