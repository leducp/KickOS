// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Kernel synchronization primitives. Blocking funnels through the scheduler's single
// reschedule() point; a post readies the highest-priority waiter and hands it the token
// directly, so a post from ISR context can drive an immediate interrupt-exit switch to
// the woken thread.

#ifndef KICKOS_SYNC_H
#define KICKOS_SYNC_H

#include <kickos/thread.h>

#include <kickos/sys/errno.h> // KOS_E* codes (mutex/sem returns speak the fleet taxonomy)

namespace kickos
{
    // A wait queue is a List of BLOCKED threads, using the shared TCB link node: a thread
    // is on the ready list XOR on one of these, never both.
    struct Semaphore
    {
        int count = 0;
        List waiters;
    };

    // Priority-inheritance mutex (CAP_MUTEX). `owner != nullptr` IS the lock state; there
    // is deliberately no `locked` bool. next_held links this mutex into its owner's
    // held-mutex chain (Thread::held_list), which is what a release recomputes the owner's
    // effective priority over. Owner-died reaches the woken waiter through its
    // Thread::wait_result, not through any per-mutex field.
    struct Mutex
    {
        Thread* owner = nullptr; // nullptr == unlocked
        List waiters;
        Mutex* next_held = nullptr; // intrusive link in the owner's held list
    };

    // wait_result value delivered to a lock() caller woken because the owner exited while
    // holding the mutex: the lock IS held, but the protected invariant may be inconsistent.
    // Negative, yet still an ACQUIRE, unlike the other negative returns.
    static constexpr intptr_t MUTEX_OWNER_DIED = -KOS_EOWNERDEAD;

    // Remove and return the highest-priority waiter (FIFO among equals), or nullptr. Pure
    // select+unlink: no state or schedule change. ISR-callable. The priority scan is lazy
    // at-pop, so a waiter boosted while parked needs no re-queue.
    Thread* wq_pop_highest(List& q);
    // The same choice WITHOUT unlinking, for a caller that must know it can complete
    // before it commits to popping. Under the same IrqLock a following wq_pop_highest
    // returns this exact thread.
    Thread* wq_peek_highest(List& q);
    // Park current on q and switch away; returns once a waker popped it and woke it.
    // Thread context only. Caller holds ONE continuous IrqLock across the block decision
    // AND this call; that is what makes it lost-wake-free.
    void wq_block(List& q);

    // Resume barrier, MANDATORY for any blocking primitive that reads waker-set TCB state
    // (wait_result) after resuming:
    //     Thread* c = sched::current(); uint64_t epoch;
    //     { IrqLock lock; ...predicate + set up state...; epoch = c->switch_count;
    //       wq_block(q); }                          // lock RELEASED here
    //     wq_confirm_resume(c, epoch);              // <- barrier, OUTSIDE the lock
    //     use c->wait_result;                       // now guaranteed post-resume
    // On ARM arch_switch only PENDS PendSV and arch_irq_restore has no ISB, so a few
    // instructions retire on the not-yet-switched thread after the block scope's lock
    // drops, and a wait_result read there returns the PRE-block value. No-op on the sim,
    // whose switch is synchronous. The waker, never the sleeper, must write wait_result
    // and clear blocked_on under the lock.
    void wq_confirm_resume(Thread* c, uint64_t epoch);

    void sem_init(Semaphore* s, int initial);
    void sem_wait(Semaphore* s);
    bool sem_trywait(Semaphore* s); // non-blocking; true if token taken
    // Hands the token to the highest-priority waiter, else banks it. Safe from thread or
    // ISR context. Returns false only with no waiter and the count at KOS_SEM_COUNT_MAX:
    // incrementing an `int` past its range is undefined, so the post is refused and the
    // count left alone. The syscall reports -KOS_EOVERFLOW; the ISR posters ignore it.
    bool sem_post(Semaphore* s);

    // Priority-inheritance mutex, thread context only. THE LOCKING CONTRACT DIFFERS BY
    // CALL. mutex_unlock and mutex_force_unlock do their whole job under an IrqLock and
    // nest fine under a caller-held one. mutex_lock MUST NOT be called with a caller-held
    // IrqLock spanning it: it takes its own lock for the acquire/park only, then RELEASES
    // it and runs the resume barrier and wait_result read outside any lock. A spanning
    // caller lock keeps BASEPRI raised past that read and reintroduces the ARM stale-read
    // bug (see wq_confirm_resume).
    void mutex_init(Mutex* m);
    // Returns 0 when locked; -KOS_EOWNERDEAD when handed the mutex by a dying owner, where
    // the lock IS held and this is NOT a failed acquire; or -KOS_EDEADLK when the acquire
    // would deadlock (self-lock or a wait cycle), refused WITHOUT parking and WITHOUT
    // leaking a boost. The -KOS_EBADF bad-cap reject happens at the syscall resolve.
    int mutex_lock(Mutex* m);
    // Hands off to the highest waiter, then re-establishes the ex-owner's effective
    // priority by recompute over what it still holds. Returns 0, or -KOS_EPERM if the
    // caller is not the owner, which is a user-triggerable runtime error and never a panic.
    int mutex_unlock(Mutex* m);
    // For a dying owner (Thread::dying, mid-cap_teardown): force-unlock, delivering
    // MUTEX_OWNER_DIED to the woken waiter. Deliberately does NOT recompute the dying
    // thread's priority; it has only its own teardown left to run.
    void mutex_force_unlock(Mutex* m, Thread* dying);

    // Link/unlink a CALL_REPLY_WAIT caller on the server's reply-donor list. MUST be
    // called at every reply-cap mint site and at every site that consumes a CAP_REPLY
    // entry, or the list and the table entries drift apart. Caller holds IrqLock.
    void reply_donor_park(Thread* server, Thread* caller);
    void reply_donor_unpark(Thread* server, Thread* caller);

    // The single effective-priority recompute funnel. t's effective prio is the max of its
    // base_prio, the highest waiter across every mutex it holds, the prio of every caller
    // parked on t->reply_waiters, and the highest parked SEND_WAIT caller on each endpoint
    // where ep->server == t. NEVER a restore-to-base: a revert must respect every live
    // donor, so a mutex unlock mid-transaction cannot deflate a live call donation.
    // Caller holds IrqLock.
    uint8_t thread_effective_prio(Thread* t);
}

#endif
