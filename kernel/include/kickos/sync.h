// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Kernel synchronization primitives. Blocking funnels through the scheduler's
// single reschedule() point; a post readies the highest-priority waiter and
// hands it the token directly, so a post from ISR context drives an immediate
// (interrupt-exit) switch to the woken thread -- scheduler trigger #3 (thread
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

    // Priority-inheritance mutex (CAP_MUTEX). `owner != nullptr` IS the lock state
    // (no redundant `locked` bool). waiters is a waitq. next_held links this mutex
    // into its owner's held-mutex chain (Thread::held_list) so a release can
    // recompute the owner's effective priority over everything it still holds.
    // Owner-died is delivered through the woken waiter's Thread::wait_result, not a
    // per-mutex bool.
    struct Mutex
    {
        Thread* owner = nullptr; // nullptr == unlocked
        List waiters;
        Mutex* next_held = nullptr; // intrusive link in the owner's held list
    };

    // wait_result value delivered to a lock() caller woken because the owner exited
    // while holding the mutex (mirrors POSIX EOWNERDEAD): the protected invariant may
    // be inconsistent. Must match KOS_MUTEX_OWNER_DIED in the user ABI.
    static constexpr intptr_t MUTEX_OWNER_DIED = 1;

    // The waitq primitive shared by every blocking object (sem today; mutex/endpoint
    // later). A waitq IS a List of BLOCKED threads; these are its only two operations.
    // Remove+return the highest-priority waiter (FIFO among equals), or nullptr; pure
    // select+unlink, no state/schedule change. ISR-callable (sem_post uses it). The
    // priority scan is lazy at-pop so a waiter boosted while parked needs no re-queue.
    Thread* wq_pop_highest(List& q);
    // Park current on q and switch away; returns once a waker popped it and woke it.
    // Thread context only; caller holds ONE continuous IrqLock across the block
    // decision AND this call (lost-wake freedom).
    void wq_block(List& q);

    // Resume barrier for a blocking primitive that reads waker-set TCB state
    // (wait_result) after resuming. THE PATTERN, reused by every such caller
    // (mutex_lock today; endpoint recv, #4):
    //     Thread* c = sched::current(); uint64_t epoch;
    //     { IrqLock lock; ...predicate + set up state...; epoch = c->switch_count;
    //       wq_block(q); }                          // lock RELEASED here
    //     wq_confirm_resume(c, epoch);              // <- barrier, OUTSIDE the lock
    //     use c->wait_result;                       // now guaranteed post-resume
    // Why it is mandatory: on ARM arch_switch only PENDS PendSV and arch_irq_restore
    // has no ISB, so after the block scope's lock drops a few instructions retire on
    // the not-yet-switched thread before the switch lands. Reading wait_result there
    // returns the PRE-block value. This spins until switch_count advances (the thread
    // is really switched back in). No-op on the sim (synchronous switch). The waker
    // must write wait_result AND clear blocked_on under the lock, never the sleeper.
    void wq_confirm_resume(Thread* c, uint64_t epoch);

    void sem_init(Semaphore* s, int initial);
    void sem_wait(Semaphore* s);
    bool sem_trywait(Semaphore* s); // non-blocking; true if token taken
    void sem_post(Semaphore* s);    // safe from thread or ISR context

    // Priority-inheritance mutex, thread context only. LOCKING CONTRACT differs by
    // call: mutex_unlock and mutex_force_unlock do their whole job under an IrqLock
    // and nest fine under any caller-held lock. mutex_lock MUST NOT be called with a
    // caller-held IrqLock spanning it: it takes its own lock only for the acquire/
    // park, then RELEASES it and runs the resume barrier + wait_result read outside
    // any lock. A continuous caller lock would keep BASEPRI raised past that read and
    // reintroduce the ARM stale-read bug (see wq_confirm_resume). The syscall path
    // resolves the cap under a short separate lock, then calls mutex_lock lockless.
    void mutex_init(Mutex* m);
    // Acquire. Uncontended: take it (two stores, no prio work). Contended: run a
    // two-pass chain walk (pass 1 cycle/deadlock detection read-only, pass 2 PI
    // boost), then park. Returns 0 locked, MUTEX_OWNER_DIED (1) if handed the mutex
    // by a dying owner, or -2 if the acquire would deadlock (self-lock or a wait
    // cycle) -- refused WITHOUT parking and WITHOUT leaking a boost.
    int mutex_lock(Mutex* m);
    // Release + hand-off to the highest waiter, then re-establish the ex-owner's
    // effective priority by recompute over its remaining held mutexes. Returns 0, or
    // -1 if the caller is not the owner (a user-triggerable runtime error, never a
    // panic once syscall-exposed).
    int mutex_unlock(Mutex* m);
    // Exit teardown (R3): the owning thread is EXITED; force-unlock, delivering
    // MUTEX_OWNER_DIED to the woken waiter. No recompute for the dying thread.
    void mutex_force_unlock(Mutex* m, Thread* dying);
}

#endif
