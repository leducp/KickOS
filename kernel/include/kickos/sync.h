// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Kernel synchronization primitives: semaphores, priority-inheritance mutexes, and the
// wait-queue park and resume calls.

#ifndef KICKOS_SYNC_H
#define KICKOS_SYNC_H

#include <kickos/thread.h>

#include <kickos/sys/errno.h>
#include <kickos/held.h>

namespace kickos
{
    class IrqLock;

    // A wait queue is a List of BLOCKED threads, using the shared TCB link node: a thread
    // is on the ready list XOR on one of these.
    struct Semaphore
    {
        int count = 0;
        List waiters;
    };

    // Priority-inheritance mutex (CAP_MUTEX). `owner != nullptr` is the whole lock state. An
    // owner-died wake reaches the waiter only through its Thread::wait_result.
    struct Mutex
    {
        Thread* owner = nullptr;
        List waiters;
        Mutex* next_held = nullptr; // intrusive link in the owner's held list
    };

    // Delivered as wait_result to a lock() caller woken by an owner that exited holding the
    // mutex: the lock is held, but the protected invariant may be inconsistent. Negative and
    // still an acquire, unlike every other negative return.
    static constexpr intptr_t MUTEX_OWNER_DIED = -KOS_EOWNERDEAD;

    // Remove and return the highest-priority waiter (FIFO among equals), or nullptr. Under the
    // caller's IrqLock, which an ISR already holds. Pure select+unlink, with no state or
    // schedule change. The priority scan is lazy at-pop, so a waiter boosted while parked needs no
    // re-queue.
    Thread* wq_pop_highest(List& q);
    // The same choice without unlinking. Under the same IrqLock a following wq_pop_highest
    // returns this exact thread.
    Thread* wq_peek_highest(List& q);
    // What every park asks for, minted only by park_cancel_pending.
    class ParkToken
    {
    public:
        [[nodiscard]] bool cancelled() const
        {
            return c_->cancel_kind != CANCEL_NONE and not c_->dying;
        }

    private:
        explicit ParkToken(Thread const* c) : c_(c)
        {
        }

        friend ParkToken park_cancel_pending(Thread const* c);

        Thread const* c_;
    };

    // Ask under IrqLock before any side effect in a blocking operation, and heed cancelled():
    // a cancel raised after syscall entry checked it but before the thread is BLOCKED leaves
    // no waiter to wake. Do not move the ask into the park itself: a transaction may already be
    // committed. The answer stays valid until the lock releases.
    [[nodiscard]] inline ParkToken park_cancel_pending(Thread const* c)
    {
        return ParkToken(c);
    }

    using WqBlock = uint32_t(List& q, WaitKind kind, void* obj, Thread* woken, Held held);
    using ParkQueueless = uint32_t(Thread* c, WaitKind kind, void* obj, Held held);

    // wq_block(ask)(q, kind, obj, woken, held): park current on q and switch away. Thread context
    // only. Hold one continuous IrqLock across the block decision and this call to avoid lost
    // wakes. kind and obj identify the queue for cancellation and timeout cleanup.
    // wq_pop_highest clears them; re-parking must set them again. Pass any thread readied by
    // wake_no_resched as woken so the park's reschedule places it if this core does not select
    // it. Returns the epoch for wq_confirm_resume.
    inline WqBlock& wq_block(ParkToken const&)
    {
        // Declared only here and at its definition, so no park reaches it without a token.
        WqBlock wq_block;
        return wq_block;
    }

    // park_queueless(ask)(c, kind, obj, held): park `current` on no list at all. The wait edge
    // is then the only thing that can find it again, so `kind` must be a kind some waker sweeps
    // for (thread.h). Detaches from the ready set without rescheduling, so the caller decides
    // when to switch away and may link the thread somewhere else first. The waker writes
    // wait_result and clears the edge before waking; a parked thread never writes its own
    // result. Returns the epoch for wq_confirm_resume, sampled before anything the caller does
    // next can switch.
    inline ParkQueueless& park_queueless(ParkToken const&)
    {
        // As wq_block's.
        ParkQueueless park_queueless;
        return park_queueless;
    }

    // Park the calling writer while the kernel console is dark (console_dark), with no
    // deadline: the reclaim and a publish end the wait (console_dark_wake). 0 once the console
    // is no longer dark, or -KOS_ECANCELED where the caller is cancelled, before or during it.
    // Caller holds no lock.
    int console_dark_wait(void);
    // Wake every writer console_dark_wait or console_room_wait parked.
    void console_dark_wake(Held held);

    // The ring refused the calling writer's line: on a backend with no TX interrupt the writer
    // drains it itself until the line fits, and on one whose TX interrupt drains it the writer
    // parks with no deadline until that drain frees the room (console_tx_room_freed), a publish
    // or a reclaim. 0 to offer the line again, or -KOS_ECANCELED where the caller is cancelled,
    // before or during it. Caller holds no lock.
    int console_room_wait(char const* buf, size_t n, int crlf);

    // Park the calling writer for `timeout_us` while a peer node's claim on a shared UART refuses
    // its line: nothing marks the end of that claim. 0 to offer the line again, at the timeout or
    // at a console_dark_wake, or -KOS_ECANCELED where the caller is cancelled, before or during it.
    // Caller holds no lock.
    int console_claim_wait(uint32_t timeout_us);

    // After blocking, release IrqLock and call wq_confirm_resume before reading
    // waker-written state such as wait_result:
    //   { IrqLock lock; epoch = wq_block(ask)(q, kind, obj, nullptr); }
    //   wq_confirm_resume(c, epoch);
    //   result = c->wait_result;
    // ARM may execute instructions after unmasking before PendSV runs. The barrier
    // prevents reading pre-block state. It is a no-op on synchronous-switch backends.
    void wq_confirm_resume(Thread* c, uint32_t epoch);

    void sem_init(Semaphore* s, int initial);
    // `held` is the caller's outermost bracket, spanning the resolve that produced `s`; a
    // cancel honoured here ends it (sched::exit_current). Returns false with a token taken,
    // or true once parked with `epoch` sampled: the caller then leaves `held`'s scope, calls
    // wq_confirm_resume and reads wait_result (0 handed a token, -KOS_ECANCELED cancelled).
    bool sem_wait(IrqLock& held, Semaphore* s, uint32_t& epoch);
    // Hands the token to the highest-priority waiter, else banks it. Thread or ISR context.
    // Returns false only with no waiter and the count already at KOS_SEM_COUNT_MAX, where the
    // post is refused and the count left alone. The syscall reports -KOS_EOVERFLOW.
    bool sem_post(Semaphore* s);

    // Priority-inheritance mutex, thread context only. The locking contract differs by call:
    // mutex_unlock does its whole job under an IrqLock and nests fine under a caller-held
    // one. mutex_force_unlock takes none of its own: caller holds the exclusion. mutex_lock
    // must not be called with a caller-held IrqLock spanning it: it takes its own lock for
    // the acquire/park only, then releases it and runs the resume barrier and wait_result
    // read outside any lock. A spanning caller lock keeps BASEPRI raised past that read
    // (see wq_confirm_resume).
    void mutex_init(Mutex* m);
    // Returns 0 when locked; -KOS_EOWNERDEAD when handed the mutex by a dying owner, where
    // the lock is held and this is not a failed acquire; or -KOS_EDEADLK when the acquire
    // would deadlock (self-lock or a wait cycle), refused without parking and without
    // leaking a boost. The -KOS_EBADF bad-cap reject happens at the syscall resolve.
    int mutex_lock(Mutex* m);
    // Hands off to the highest waiter, then recomputes the ex-owner's effective priority over
    // what it still holds. Returns 0, or -KOS_EPERM if the caller is not the owner.
    int mutex_unlock(Mutex* m);
    // For a dying owner (Thread::dying, mid-cap_teardown): force-unlock, delivering
    // MUTEX_OWNER_DIED to the woken waiter, leaving the dying thread's priority unrecomputed.
    void mutex_force_unlock(Mutex* m, Thread* dying, Held held);

    // Link/unlink a CALL_REPLY_WAIT caller on the server's reply-donor list. Must be
    // called at every reply-cap mint site and at every site that consumes a CAP_REPLY
    // entry, or the list and the table entries drift apart. Caller holds IrqLock.
    void reply_donor_park(Thread* server, Thread* caller);
    // False means `caller` was not on this server's list and nothing was touched: a stale reply
    // cap can resolve to a caller parked on a different server. A false return forbids every
    // step that follows an unpark (delivering into the caller's buffer, writing its wait_result
    // or call_state, waking it): the caller is still parked there and `link` is that list's.
    bool reply_donor_unpark(Thread* server, Thread* caller);

    // The only writers of Endpoint::server on a live endpoint, keeping that field and the
    // server's served-endpoint chain in step; endpoint_create seats both itself. set()
    // re-seats: it unlinks `ep` from a previous server first, so recv may call it on every
    // arrival. Caller holds IrqLock.
    void endpoint_server_set(Endpoint* ep, Thread* t);
    void endpoint_server_clear(Endpoint* ep);

    // A receiver entering a receive on `ep`: it becomes the server and the endpoint is no
    // longer vacated. Caller holds IrqLock.
    void endpoint_receiver_waits(Endpoint* ep, Thread* t);

    // A pool slot reset to a fresh endpoint, its index, or -1 with *out unchanged when the pool
    // is full. Caller holds IrqLock.
    [[nodiscard]] int endpoint_slot_claim(Endpoint** out);
    // The same, seated for its creator, whose cap carries every right: a receiver and a
    // handout holder from the start, so the endpoint is never vacated before its first close.
    [[nodiscard]] int endpoint_claim_created(Endpoint** out);

    // The single effective-priority recompute funnel. t's effective prio is the max of its
    // base_prio, the highest waiter across every mutex it holds, the prio of every caller
    // parked on t->reply_waiters, and the highest parked SEND_WAIT caller on each endpoint
    // where ep->server == t. Never a restore-to-base: a mutex unlock mid-transaction must not
    // deflate a live call donation. Every term is O(donors) and none is bounded by a configured
    // pool capacity, and this runs interrupt-masked on every mutex unlock, reply and close.
    // Caller holds IrqLock.
    uint8_t thread_effective_prio(Thread* t);
}

// The console's driver-death reclaim (kickos/console_tx.h notes the death). Puts the UART back in
// a known polled state so panic and ordinary kprintf still reach the wire, and wakes every
// writer waiting out the dark window. Idempotent, and a no-op if the console was never
// published, if no death is noted, while a publish is handing over (its set_user acts on the
// note), or while ANY live thread still holds arch_console_reclaim_window(). Only a thread
// DEATH can free that window, so a refusal is retried at the next death.
void console_on_driver_death(kickos::Held held);

#endif
