// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Kernel synchronization primitives. Blocking funnels through the scheduler's
// single reschedule() point; a post readies the highest-priority waiter and
// hands it the token directly, so a post from ISR context drives an immediate
// (interrupt-exit) switch to the woken thread — scheduler trigger #3 (thread
// ctx) and #4 (IRQ ctx).

#ifndef KICKOS_SYNC_H
#define KICKOS_SYNC_H

#include <kickos/thread.h>

namespace kickos
{
    // A wait queue is just a List of blocked threads (the shared TCB link node);
    // the only wait-queue-specific policy is the highest-priority scan on removal
    // (wq_pop_highest in sync.cc). A thread is on the ready list XOR one of these.
    struct Semaphore
    {
        int count = 0;
        List waiters;
    };

    struct Mutex
    {
        bool locked = false;
        Thread* owner = nullptr;
        List waiters;
    };

    void sem_init(Semaphore* s, int initial);
    void sem_wait(Semaphore* s);
    bool sem_trywait(Semaphore* s); // non-blocking; true if token taken
    void sem_post(Semaphore* s);    // safe from thread or ISR context

    void mutex_init(Mutex* m);
    void mutex_lock(Mutex* m);
    void mutex_unlock(Mutex* m);
}

#endif
