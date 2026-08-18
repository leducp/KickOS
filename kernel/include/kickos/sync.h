// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Kernel synchronization primitives: semaphores, priority-inheritance mutexes, and the
// wait-queue park and resume calls.

#ifndef KICKOS_SYNC_H
#define KICKOS_SYNC_H

#include <kickos/thread.h>

#include <kickos/sys/errno.h> // KOS_E* codes

namespace kickos
{
    // A wait queue is a List of BLOCKED threads, using the shared TCB link node: a thread
    // is on the ready list XOR on one of these, never both.
    struct Semaphore
    {
        int count = 0;
        List waiters;
    };

    // Priority-inheritance mutex (CAP_MUTEX). `owner != nullptr` IS the lock state, and there
    // is no second one. An owner-died wake reaches the waiter through its Thread::wait_result
    // and through no field here.
    struct Mutex
    {
        Thread* owner = nullptr;
        List waiters;
        Mutex* next_held = nullptr; // intrusive link in the owner's held list
    };

    // Delivered as wait_result to a lock() caller woken by an owner that exited holding the
    // mutex: the lock IS held, but the protected invariant may be inconsistent. Negative and
    // still an ACQUIRE, unlike every other negative return.
    static constexpr intptr_t MUTEX_OWNER_DIED = -KOS_EOWNERDEAD;

    // Remove and return the highest-priority waiter (FIFO among equals), or nullptr. Pure
    // select+unlink: no state or schedule change. ISR-callable. The priority scan is lazy
    // at-pop, so a waiter boosted while parked needs no re-queue.
    Thread* wq_pop_highest(List& q);
    // The same choice WITHOUT unlinking. Under the same IrqLock a following wq_pop_highest
    // returns this exact thread.
    Thread* wq_peek_highest(List& q);
    // Park current on q and switch away; returns once a waker popped it and woke it. Thread
    // context only. Caller holds ONE continuous IrqLock across the block decision AND this
    // call, or the wake is lost.
    //
    // `kind` and `obj` are the wait edge (thread.h): `obj` must be the object owning `q`, and
    // the pair must name the list this call parks on; thread_kill and the timed unwind reach
    // that list through the tag and nothing else. wq_pop_highest clears both, so a re-park
    // onto another list has to re-state them.
    void wq_block(List& q, WaitKind kind, void* obj);

    // Park `current` on NO list at all: the wait edge is then the ONLY thing that can find it
    // again, so `kind` must be a kind some waker sweeps for (thread.h). Detaches from the
    // ready set WITHOUT rescheduling, so the caller decides when to switch away and may link
    // the thread somewhere else first. The waker writes wait_result and clears the edge before
    // waking; a parked thread never writes its own result. Caller holds IrqLock.
    void park_queueless(Thread* c, WaitKind kind, void* obj);

    // Resume barrier, MANDATORY for any blocking primitive that reads waker-set TCB state
    // (wait_result) after resuming:
    //     Thread* c = sched::current(); uint32_t epoch;
    //     { IrqLock lock; ...predicate + set up state...; epoch = c->switch_count;
    //       wq_block(q, kind, obj); }              // lock RELEASED here
    //     wq_confirm_resume(c, epoch);              // barrier, OUTSIDE the lock
    //     use c->wait_result;                       // now guaranteed post-resume
    // On ARM arch_switch only PENDS PendSV and arch_irq_restore has no ISB, so a wait_result
    // read after the block scope's lock drops can still retire on the not-yet-switched thread
    // and return the PRE-block value. No-op on the sim, whose switch is synchronous. The
    // waker, never the sleeper, writes wait_result and clears the wait edge under the lock.
    void wq_confirm_resume(Thread* c, uint32_t epoch);

    void sem_init(Semaphore* s, int initial);
    void sem_wait(Semaphore* s);
    bool sem_trywait(Semaphore* s); // non-blocking; true if token taken
    // Hands the token to the highest-priority waiter, else banks it. Thread or ISR context.
    // Returns false only with no waiter and the count already at KOS_SEM_COUNT_MAX, where the
    // post is refused and the count left alone. The syscall reports -KOS_EOVERFLOW.
    bool sem_post(Semaphore* s);

    // Priority-inheritance mutex, thread context only. THE LOCKING CONTRACT DIFFERS BY
    // CALL. mutex_unlock and mutex_force_unlock do their whole job under an IrqLock and
    // nest fine under a caller-held one. mutex_lock MUST NOT be called with a caller-held
    // IrqLock spanning it: it takes its own lock for the acquire/park only, then RELEASES
    // it and runs the resume barrier and wait_result read outside any lock. A spanning
    // caller lock keeps BASEPRI raised past that read (see wq_confirm_resume).
    void mutex_init(Mutex* m);
    // Returns 0 when locked; -KOS_EOWNERDEAD when handed the mutex by a dying owner, where
    // the lock IS held and this is NOT a failed acquire; or -KOS_EDEADLK when the acquire
    // would deadlock (self-lock or a wait cycle), refused WITHOUT parking and WITHOUT
    // leaking a boost. The -KOS_EBADF bad-cap reject happens at the syscall resolve.
    int mutex_lock(Mutex* m);
    // Hands off to the highest waiter, then recomputes the ex-owner's effective priority over
    // what it still holds. Returns 0, or -KOS_EPERM if the caller is not the owner.
    int mutex_unlock(Mutex* m);
    // For a dying owner (Thread::dying, mid-cap_teardown): force-unlock, delivering
    // MUTEX_OWNER_DIED to the woken waiter. Does NOT recompute the dying thread's priority.
    void mutex_force_unlock(Mutex* m, Thread* dying);

    // Link/unlink a CALL_REPLY_WAIT caller on the server's reply-donor list. MUST be
    // called at every reply-cap mint site and at every site that consumes a CAP_REPLY
    // entry, or the list and the table entries drift apart. Caller holds IrqLock.
    void reply_donor_park(Thread* server, Thread* caller);
    // False means `caller` was NOT on this server's list and NOTHING was touched: a stale reply
    // cap can resolve to a caller parked on a DIFFERENT server. A false return forbids every
    // step that follows an unpark (delivering into the caller's buffer, writing its wait_result
    // or call_state, waking it): the caller is still parked there and `link` is that list's.
    bool reply_donor_unpark(Thread* server, Thread* caller);

    // The ONLY writers of Endpoint::server, keeping that field and the server's
    // served-endpoint chain in step. set() re-seats: it unlinks `ep` from a previous server
    // first, so recv may call it on every arrival. Caller holds IrqLock.
    void endpoint_server_set(Endpoint* ep, Thread* t);
    void endpoint_server_clear(Endpoint* ep);

    // The single effective-priority recompute funnel. t's effective prio is the max of its
    // base_prio, the highest waiter across every mutex it holds, the prio of every caller
    // parked on t->reply_waiters, and the highest parked SEND_WAIT caller on each endpoint
    // where ep->server == t. NEVER a restore-to-base: a mutex unlock mid-transaction must not
    // deflate a live call donation. Every term is O(donors) and none is bounded by a configured
    // pool capacity, and this runs interrupt-masked on every mutex unlock, reply and close.
    // Caller holds IrqLock.
    uint8_t thread_effective_prio(Thread* t);
}

#endif
