// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/sync.h>
#include <kickos/sched.h>
#include <kickos/irqlock.h>

namespace kickos
{
    namespace
    {

        void wq_push_back(WaitQueue& q, Thread* t)
        {
            t->qnext = nullptr;
            t->qprev = q.tail;
            if (q.tail != nullptr) q.tail->qnext = t;
            else q.head = t;
            q.tail = t;
        }

        void wq_unlink(WaitQueue& q, Thread* t)
        {
            if (t->qprev != nullptr) t->qprev->qnext = t->qnext;
            else q.head = t->qnext;
            if (t->qnext != nullptr) t->qnext->qprev = t->qprev;
            else q.tail = t->qprev;
            t->qnext = nullptr;
            t->qprev = nullptr;
        }

        // Remove and return the highest-priority waiter (FIFO among equal priority).
        Thread* wq_pop_highest(WaitQueue& q)
        {
            Thread* best = q.head;
            if (best == nullptr) return nullptr;
            for (Thread* t = best->qnext; t != nullptr; t = t->qnext)
            {
                if (t->prio > best->prio) best = t;
            }
            wq_unlink(q, best);
            best->wait_queue = nullptr;
            return best;
        }

        // Park the current thread on `q` and switch away; returns when woken.
        void block_on(WaitQueue& q)
        {
            Thread* c = sched::current();
            c->state = ThreadState::BLOCKED;
            c->wait_queue = &q;
            wq_push_back(q, c);
            sched::block_current(); // drops from run set + reschedules; returns on wake
        }

    }

    // --- Semaphore -------------------------------------------------------------
    void sem_init(Semaphore* s, int initial)
    {
        s->count = initial;
        s->waiters = WaitQueue{};
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
    void mutex_init(Mutex* m)
    {
        m->locked = false;
        m->owner = nullptr;
        m->waiters = WaitQueue{};
    }

    void mutex_lock(Mutex* m)
    {
        IrqLock lock;
        if (!m->locked)
        {
            m->locked = true;
            m->owner = sched::current();
            return;
        }
        block_on(m->waiters);
        // On wake we are the new owner (ownership transferred by mutex_unlock).
    }

    void mutex_unlock(Mutex* m)
    {
        IrqLock lock;
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
