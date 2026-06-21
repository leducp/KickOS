// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/sync.h>
#include <kickos/sched.h>
#include <kickos/kernel.h>
#include <kickos/irqlock.h>

namespace kickos
{
    namespace
    {
        // Remove and return the highest-priority waiter (FIFO among equal priority).
        // The list mechanics are shared (List); the priority scan is the only
        // wait-queue-specific policy on top.
        Thread* wq_pop_highest(List& q)
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
            q.unlink(&best->link);
            best->wait_queue = nullptr;
            return best;
        }

        // Park the current thread on `q` and switch away; returns when woken.
        void block_on(List& q)
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
    }

    // --- Semaphore -------------------------------------------------------------
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
        block_on(s->waiters);
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

    void sem_post(Semaphore* s)
    {
        IrqLock lock;
        Thread* w = wq_pop_highest(s->waiters);
        if (w != nullptr)
        {
            sched::wake(w); // token handed directly to the woken waiter
            return;
        }
        s->count++;
    }

    // --- Mutex (binary; priority inheritance deferred, base_prio reserved) -----
    // In-kernel scaffolding: no syscall wires it to userspace yet, and it is not
    // exercised by any test. Recursive lock self-deadlocks (not re-entrant).
    void mutex_init(Mutex* m)
    {
        m->locked = false;
        m->owner = nullptr;
        m->waiters = List{};
    }

    void mutex_lock(Mutex* m)
    {
        IrqLock lock;
        if (not m->locked)
        {
            m->locked = true;
            m->owner = sched::current();
            return;
        }
        // wq_pop_highest wakes the most urgent waiter, but m->owner is NOT boosted here:
        // priority inversion is unbounded until inheritance lands (base_prio = restore-to slot).
        block_on(m->waiters);
        // On wake we are the new owner (ownership transferred by mutex_unlock).
    }

    void mutex_unlock(Mutex* m)
    {
        IrqLock lock;
        KICKOS_ASSERT(m->owner == sched::current()); // only the owner may unlock
        Thread* w = wq_pop_highest(m->waiters);
        if (w != nullptr)
        {
            m->owner = w; // transfer ownership; stays locked
            sched::wake(w);
            return;
        }
        m->locked = false;
        m->owner = nullptr;
    }
}
