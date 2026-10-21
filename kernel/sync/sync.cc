// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/sync.h>
#include <kickos/sched.h>
#include <kickos/smptrace.h>
#include <kickos/cap.h>
#include <kickos/domain.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/irqlock.h>
#include <kickos/klock.h>
#include <kickos/time.h>
#include <kickos/vrange.h>
#include <kickos/word.h>

#include <kickos/sys/abi.h>

#include <limits.h>

namespace kickos
{
    Thread* wq_pop_highest(List& q)
    {
        KICKOS_ASSERT_EXCLUSION_HELD();
        Thread* best = wq_peek_highest(q);
        // The search is recorded whether or not it found anything: an absent record says
        // nobody looked, a different finding from looking and coming back empty.
        KOS_TRACE(::kickos::KOS_TR_SEARCH, KOS_TRACE_ID(&q), KOS_TRACE_ID(best));
        if (best == nullptr)
        {
            // The head too, so a queue that is genuinely empty is told apart from a search
            // that read a queue the parked thread is not on.
            KOS_TRACE(::kickos::KOS_TR_EMPTY, KOS_TRACE_ID(&q), KOS_TRACE_ID(q.head));
        }
        if (best != nullptr)
        {
            q.unlink(&best->link);
            best->clear_wait_edge();
            // Keep the deadline when moving a caller from SEND_WAIT to reply_donor_park.
            // It covers both phases; wake_no_resched cancels it when the caller is woken.
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

    // switch_to bumps the incoming thread's switch_count, so an advance is proof of a real
    // switch-in and the acquire load is what makes the waker's writes readable.
    void wq_confirm_resume(Thread* c, uint32_t epoch)
    {
        // Reaching the cap means the switch is never coming (a masked or lost pend).
        uint32_t spin = 0;
        while (c->switch_count.load() == epoch)
        {
            if (++spin > KICKOS_POLL_SPIN_MAX)
            {
                kpanic(diag::kResumeNoSwitch);
            }
        }
    }

    uint32_t park_queueless(Thread* c, WaitKind kind, void* obj, Held held)
    {
        // BLOCKED before the detach: on_remove reads `state` to tell a park from a set_prio
        // re-seat, and only a park forfeits the RR slice remainder. The removal reads `link`,
        // so it must run before anything re-uses that node.
        c->state.block();
        sched::detach_current(held);
        c->wait_queue = nullptr;
        c->wait_kind = kind;
        c->wait_obj = obj;
        return c->switch_count;
    }

    uint32_t wq_block(List& q, WaitKind kind, void* obj, Thread* woken, Held held)
    {
        Thread* c = sched::current();
        uint32_t const epoch = park_queueless(c, kind, obj, held);
        c->wait_queue = &q;
        q.push_back(&c->link);
        // BEFORE the reschedule, which on a stall never returns.
        KOS_TRACE(::kickos::KOS_TR_PARK, KOS_TRACE_ID(c), KOS_TRACE_ID(&q));
        sched::reschedule(woken, held);
        return epoch;
    }

    static_assert(KOS_SEM_COUNT_MAX <= INT_MAX,
                  "KOS_SEM_COUNT_MAX must fit Semaphore::count (an int)");

    void sem_init(Semaphore* s, int initial)
    {
        s->count = initial;
        s->waiters = List{};
    }

    SemWait sem_wait(IrqLock& held, Semaphore* s, uint32_t timeout_us, uint32_t& epoch)
    {
        Thread* const c = sched::current();
        ParkToken const ask = park_cancel_pending(c);
        if (ask.cancelled())
        {
            sched::exit_current(KOS_EXIT_CANCELLED, sched::EXIT_RETURN, &held);
        }
        if (s->count > 0)
        {
            s->count--;
            return SemWait::TAKEN;
        }
        if (timeout_us == 0)
        {
            return SemWait::EMPTY;
        }
        ktime_deadline_arm(c, timeout_us, held);
        epoch = wq_block(ask)(s->waiters, WAIT_SEM, s, nullptr, held);
        return SemWait::PARKED;
    }

    bool sem_post(Semaphore* s)
    {
        IrqLock lock;
        Thread* w = wq_pop_highest(s->waiters);
        if (w != nullptr)
        {
            w->wait_result = 0; // the token goes straight to the waiter, count stays put
            sched::wake(w, lock);
            return true;
        }
        if (s->count >= KOS_SEM_COUNT_MAX)
        {
            return false; // refuse rather than wrap a signed int (UB)
        }
        s->count++;
        return true;
    }

    // sched::set_prio is the sole writer of an effective priority. Inheritance stops at
    // semaphores: a thread blocked on a sem answers nullptr from wait_mutex(), so the chain
    // walk stops there.
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

        // Caller holds IrqLock, and must already have held_remove'd m from the releaser and
        // popped w off m->waiters; that pop is what cleared w's wait edge. Both that clear
        // and `status` belong to the WAKER: where the resume is deferred, a self-clear leaves
        // a window in which a still-parked thread answers nullptr from wait_mutex() and the
        // chain walk stops short of it, losing a boost or a deadlock detection. w must be the
        // top waiter: no boost is recomputed for the waiters left behind it.
        void transfer_to(Mutex* m, Thread* w, intptr_t status)
        {
            m->owner = w;
            w->wait_result = status;
            held_push(w, m);
        }
    }

    // Unchecked preconditions: `caller` is already off the ready set and off every other
    // queue, so its `link` is free; and its CAP_REPLY is already installed in `server`'s
    // table. The cap entry and this membership must move together or the funnel miscounts.
    void reply_donor_park(Thread* server, Thread* caller)
    {
        server->reply_waiters.push(&caller->link);
        // Set the reply wait edge after the endpoint pop clears the send wait edge.
        caller->wait_kind = WAIT_EP_REPLY;
        caller->wait_obj = server;
    }

    // Must run wherever a CAP_REPLY entry is consumed (reply, voluntary close, exit teardown)
    // and BEFORE the funnel recompute that follows, else the donor is counted after its cap is
    // gone. Membership is TESTED, not assumed: cap_reply_caller matches only the low 8 bits of
    // the call sequence, so a stale reply cap held by A can resolve to a caller parked on B.
    // The false return must be branched on: `link` is ready-list XOR wait-queue XOR
    // reply-donor, so completing a transaction against a caller this did not unlink would
    // walk two lists through one node.
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
        // A biased -1 IS the sentinel, and the walk below stops on a match: a zero `self` would
        // match the sentinel and write ep->next_served over the last link field.
        KICKOS_ASSERT(index >= 0);
        uint16_t const self = ep_served_ref(index);
        // ONE bound ends this walk: EP_SERVED_NONE decodes out of range (endpoint.h), and so
        // does a ref naming no slot. Do not add a sentinel test beside it, because this frame
        // is measured on the rxv3 RET red zone and that zone has no slack.
        uint16_t* pp = &prior->served_head;
        while (*pp != self)
        {
            Endpoint* const link = kernel().endpoints.at(ep_served_index(*pp));
            if (link == nullptr)
            {
                KICKOS_ASSERT(false); // server set, yet absent from that server's chain
                return;
            }
            pp = &link->next_served;
        }
        *pp = ep->next_served;
        ep->next_served = EP_SERVED_NONE;
        ep->server = nullptr;
    }

    void endpoint_server_set(Endpoint* ep, Thread* t)
    {
        // Keeps index_of's divide off the per-message path.
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

    void endpoint_receiver_waits(Endpoint* ep, Thread* t)
    {
        endpoint_server_set(ep, t);
        ep->vacated = 0;
    }

    int endpoint_slot_claim(Endpoint** out)
    {
        int const i = kernel().endpoints.alloc();
        Endpoint* const ep = kernel().endpoints.at(i); // at(-1) returns null.
        if (ep == nullptr)
        {
            return -1;
        }
        *ep = Endpoint{};
        *out = ep;
        return i;
    }

    int endpoint_claim_created(Endpoint** out)
    {
        int const i = endpoint_slot_claim(out);
        if (i >= 0)
        {
            (*out)->recv_holders = 1;
            (*out)->handout_holders = 1;
        }
        return i;
    }

    // The single effective-priority funnel. Runs under IrqLock on every mutex unlock,
    // reply and close, so NO term added here may walk the capability table.
    uint8_t thread_effective_prio(Thread* t)
    {
        uint8_t p = t->base_prio;
        for (Mutex* h = t->held_list; h != nullptr; h = h->next_held)
        {
            Thread const* const w = wq_peek_highest(h->waiters);
            if (w != nullptr and w->prio > p)
            {
                p = w->prio;
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
        // Follows the served-endpoint chain: ep->server defines membership, and each
        // linked slot stays live until the server reference is removed. EP_SERVED_NONE
        // resolves to null and terminates traversal. Avoid panic paths here: this code
        // runs below the timer trap and console reporting exceeds its RV32 red zone. The
        // debug assertion adds such a path in debug builds.
        for (Endpoint* ep = kernel().endpoints.at(ep_served_index(t->served_head)); ep != nullptr;
             ep = kernel().endpoints.at(ep_served_index(ep->next_served)))
        {
            KICKOS_DEBUG_ASSERT(ep->server == t);
            for (ListNode* n = ep->send_waiters.head; n != nullptr; n = n->next)
            {
                Thread* s = thread_of(n);
                if (s->call_state == CALL_SEND_WAIT and s->prio > p)
                {
                    p = s->prio;
                }
            }
        }
        return p;
    }

    namespace
    {
        constexpr uint32_t WORD_OWNER = ~static_cast<uint32_t>(KOS_MUTEX_WAITERS);

        Mutex* record_find(arch_phys_addr_t key)
        {
            for (Mutex* m = kernel().mutex_active; m != nullptr; m = m->next_active)
            {
                if (m->key == key)
                {
                    return m;
                }
            }
            return nullptr;
        }

        // Never null: each waiter parks on one record, and a free record has no owner.
        Mutex* record_take(arch_phys_addr_t key, Thread* owner)
        {
            Kernel& k = kernel();
            for (Mutex& m : k.mutex_records)
            {
                if (m.owner == nullptr)
                {
                    m.key = key;
                    m.owner = owner;
                    m.waiters = List{};
                    m.prev_active = nullptr;
                    m.next_active = k.mutex_active;
                    if (k.mutex_active != nullptr)
                    {
                        k.mutex_active->prev_active = &m;
                    }
                    k.mutex_active = &m;
                    held_push(owner, &m);
                    return &m;
                }
            }
            KICKOS_UNREACHABLE(::kickos::diag::kMutexNoRecord);
        }

        // `m` has no waiter left and is off its owner's held list.
        void record_free(Mutex* m)
        {
            if (m->prev_active == nullptr)
            {
                kernel().mutex_active = m->next_active;
            }
            else
            {
                m->prev_active->next_active = m->next_active;
            }
            if (m->next_active != nullptr)
            {
                m->next_active->prev_active = m->prev_active;
            }
            m->prev_active = nullptr;
            m->next_active = nullptr;
            m->next_held = nullptr;
            m->owner = nullptr;
            m->key = 0;
        }

        // The live thread the id names, if it could have written the word: anything else is a
        // dead holder. An id cannot name idle, whose TCB sits outside the pool.
        Thread* owner_resolve(uint32_t id, WordRef const& w)
        {
            uint32_t const slot = id & LOCK_ID_SLOT_MASK;
            if (slot == 0 or slot > static_cast<uint32_t>(KICKOS_THREAD_SLOTS))
            {
                return nullptr;
            }
            Thread* const t = &kernel().threads.slots[slot - 1u];
            // INACTIVE is a slot never used or still being built, which has held nothing.
            if (t->state == ThreadState::EXITED or t->state == ThreadState::INACTIVE or t->dying
                or t->lock_id != id or not word_could_hold(t, w))
            {
                return nullptr;
            }
            return t;
        }

        // What a contended attempt ends in, decided under the lock.
        enum class Attempt : uint8_t
        {
            ANSWERED,
            PARKED,
            RETRY
        };

        // One attempt of mutex_lock, under `lock`. Every refusal and retry precedes the record
        // being taken, and taking it leads only to the park.
        Attempt lock_attempt(IrqLock& lock, Thread* c, uintptr_t va, uint32_t timeout_us,
                             int* rc, uint32_t* epoch)
        {
            // Ahead of the donation walk below, whose boosts an exit taken at the park would
            // leave seated on this caller's behalf with the caller gone.
            ParkToken const ask = park_cancel_pending(c);
            if (ask.cancelled())
            {
                sched::exit_current(KOS_EXIT_CANCELLED, sched::EXIT_RETURN, &lock);
            }
            WordRef w;
            *rc = word_ref(c, va, &w);
            if (*rc != 0)
            {
                return Attempt::ANSWERED;
            }
            Mutex* m = nullptr;
            Thread* t = nullptr;
            // The hold of the word ends before the park.
            {
                WordAccess word(w);
                if (not word.reached())
                {
                    *rc = -KOS_EFAULT;
                    return Attempt::ANSWERED;
                }
                uint32_t const v = word.load();
                m = record_find(w.key);
                if (m != nullptr)
                {
                    // The record is the authority: a word naming anyone else broke the protocol.
                    if ((v & WORD_OWNER) != m->owner->lock_id)
                    {
                        *rc = -KOS_EINVAL;
                        return Attempt::ANSWERED;
                    }
                    if (m->owner == c)
                    {
                        *rc = -KOS_EDEADLK;
                        return Attempt::ANSWERED;
                    }
                    // A writer cleared it; set again, it keeps the holder's release in the
                    // kernel.
                    uint32_t expected = v;
                    if ((v & KOS_MUTEX_WAITERS) == 0
                        and not word.cas(&expected, v | KOS_MUTEX_WAITERS))
                    {
                        return Attempt::RETRY;
                    }
                    t = m->owner;
                }
                else
                {
                    uint32_t const id = v & WORD_OWNER;
                    if (id == c->lock_id)
                    {
                        *rc = -KOS_EDEADLK;
                        return Attempt::ANSWERED;
                    }
                    if (id != 0)
                    {
                        t = owner_resolve(id, w);
                    }
                    if (t == nullptr)
                    {
                        uint32_t expected = v;
                        if (not word.cas(&expected, c->lock_id))
                        {
                            return Attempt::RETRY;
                        }
                        *rc = 0;
                        if (id != 0)
                        {
                            *rc = static_cast<int>(MUTEX_OWNER_DIED);
                        }
                        return Attempt::ANSWERED;
                    }
                }
                // Before any deadline: ktime_deadline_arm treats only KOS_TIMEOUT_NONE as none,
                // and would park a try until the next expiry.
                if (timeout_us == 0)
                {
                    *rc = -KOS_ETIMEDOUT;
                    return Attempt::ANSWERED;
                }
                // Pass 1: cycle detection, READ ONLY, so a refusal writes no boost. The depth
                // bound only stops the walk on a PRE-EXISTING foreign cycle, and counts threads:
                // an acyclic chain visits each thread once.
                {
                    Thread* o = t;
                    int depth = 0;
                    while (o != nullptr)
                    {
                        if (o == c)
                        {
                            *rc = -KOS_EDEADLK;
                            return Attempt::ANSWERED;
                        }
                        if (o->wait_mutex() == nullptr)
                        {
                            break; // runnable or parked on something else: the chain ends
                        }
                        o = o->wait_mutex()->owner;
                        depth++;
                        if (depth > KICKOS_THREAD_SLOTS)
                        {
                            break;
                        }
                    }
                }
                uint32_t expected = v;
                if (m == nullptr and (v & KOS_MUTEX_WAITERS) == 0
                    and not word.cas(&expected, v | KOS_MUTEX_WAITERS))
                {
                    return Attempt::RETRY;
                }
            }
            if (m == nullptr)
            {
                m = record_take(w.key, t);
            }
            c->wait_va = va;
            // Pass 2: the chain is now known acyclic. Stopping at the first holder already at
            // or above c's prio is sufficient; everything above it is too.
            {
                Thread* o = t;
                int depth = 0;
                while (o != nullptr)
                {
                    if (o->prio >= c->prio)
                    {
                        break;
                    }
                    sched::set_prio(o, c->prio, lock);
                    if (o->wait_mutex() == nullptr)
                    {
                        break;
                    }
                    o = o->wait_mutex()->owner;
                    depth++;
                    if (depth > KICKOS_THREAD_SLOTS)
                    {
                        break;
                    }
                }
            }
            // WAIT_MUTEX is what puts c on the chain walk above for the next blocker. A waker
            // transfers the record and writes wait_result.
            ktime_deadline_arm(c, timeout_us, lock);
            *epoch = wq_block(ask)(m->waiters, WAIT_MUTEX, m, nullptr, lock);
            return Attempt::PARKED;
        }

        Attempt unlock_attempt(IrqLock& lock, Thread* c, uintptr_t va, int* rc)
        {
            WordRef w;
            *rc = word_ref(c, va, &w);
            if (*rc != 0)
            {
                return Attempt::ANSWERED;
            }
            Mutex* m = nullptr;
            {
                WordAccess word(w);
                if (not word.reached())
                {
                    *rc = -KOS_EFAULT;
                    return Attempt::ANSWERED;
                }
                uint32_t const v = word.load();
                m = record_find(w.key);
                if (m == nullptr)
                {
                    if ((v & WORD_OWNER) != c->lock_id)
                    {
                        *rc = -KOS_EPERM;
                        return Attempt::ANSWERED;
                    }
                    // Fails only when a user write raced: no kernel path writes the word under
                    // the lock, and no take succeeds on a word that is not zero.
                    uint32_t expected = v;
                    if (not word.cas(&expected, 0u))
                    {
                        return Attempt::RETRY;
                    }
                    *rc = 0;
                    return Attempt::ANSWERED;
                }
                if (m->owner != c)
                {
                    *rc = -KOS_EPERM;
                    return Attempt::ANSWERED;
                }
                // The next holder comes from the record, never from the word.
                Thread* const next = wq_peek_highest(m->waiters);
                KICKOS_ASSERT(next != nullptr); // a record lives only while it has a waiter
                uint32_t handed = next->lock_id;
                if (m->waiters.head != m->waiters.tail)
                {
                    handed = handed | KOS_MUTEX_WAITERS;
                }
                word.store(handed);
            }
            held_remove(c, m);
            Thread* const woken = wq_pop_highest(m->waiters);
            transfer_to(m, woken, 0);
            if (m->waiters.empty())
            {
                held_remove(woken, m);
                record_free(m);
            }
            uint8_t const np = thread_effective_prio(c);
            if (np != c->prio)
            {
                sched::set_prio(c, np, lock);
            }
            sched::wake(woken, lock);
            *rc = 0;
            return Attempt::ANSWERED;
        }
    }

    int mutex_lock(uintptr_t va, uint32_t timeout_us)
    {
        Thread* const c = sched::current();
        uint32_t epoch = 0;
        while (true)
        {
            int rc = 0;
            Attempt got = Attempt::RETRY;
            {
                IrqLock lock;
                got = lock_attempt(lock, c, va, timeout_us, &rc, &epoch);
            }
            if (got == Attempt::ANSWERED)
            {
                return rc;
            }
            if (got == Attempt::PARKED)
            {
                break;
            }
        }
        // The wait_result read must be BOTH outside the critical section AND after the
        // barrier, or it returns the pre-block value.
        wq_confirm_resume(c, epoch);
        return static_cast<int>(c->wait_result);
    }

    int mutex_unlock(uintptr_t va)
    {
        Thread* const c = sched::current();
        while (true)
        {
            int rc = 0;
            Attempt got = Attempt::RETRY;
            {
                IrqLock lock;
                got = unlock_attempt(lock, c, va, &rc);
            }
            if (got == Attempt::ANSWERED)
            {
                return rc;
            }
        }
    }

    void mutex_waiter_left(Mutex* m)
    {
        if (m->waiters.empty())
        {
            held_remove(m->owner, m);
            record_free(m);
        }
    }

    void mutex_owner_exit(Thread* dying, Held held)
    {
        // `dying` gets NO recompute: it stays boosted for the remainder of its own teardown, a
        // bounded inversion since the sweep is chunked. The waiter is woken with
        // MUTEX_OWNER_DIED, the protected state may be inconsistent, but never stranded.
        while (dying->held_list != nullptr)
        {
            Mutex* const m = dying->held_list;
            held_remove(dying, m);
            Thread* const next = wq_pop_highest(m->waiters);
            KICKOS_ASSERT(next != nullptr); // a record lives only while it has a waiter
            uint32_t handed = next->lock_id;
            if (not m->waiters.empty())
            {
                handed = handed | KOS_MUTEX_WAITERS;
            }
            // A waiter can always write its word: every call removing that reach ended the
            // waits through it first, so the store cannot fail.
            {
                WordAccess word(word_ref_parked(next, m->key));
                KICKOS_ASSERT(word.reached());
                word.store(handed);
            }
            transfer_to(m, next, MUTEX_OWNER_DIED);
            if (m->waiters.empty())
            {
                held_remove(next, m);
                record_free(m);
            }
            sched::wake(next, held);
        }
    }

#if KICKOS_HAVE_ASPACE
    void mutex_reach_lost(struct arch_aspace* space, uintptr_t base, size_t bytes, Held held)
    {
        Mutex* m = kernel().mutex_active;
        while (m != nullptr)
        {
            // Saved first: the abort frees a record its last waiter leaves.
            Mutex* const next_record = m->next_active;
            ListNode* n = m->waiters.head;
            while (n != nullptr)
            {
                ListNode* const after = n->next;
                Thread* const t = thread_of(n);
                if (domain_space(thread_domain(t)) == space and t->wait_va >= base
                    and t->wait_va - base < bytes)
                {
                    thread_abort_park(t, -KOS_EFAULT, held);
                }
                n = after;
            }
            m = next_record;
        }
    }

    void mutex_windows_lost(struct arch_aspace* space, VirtualRanges const* ranges,
                            uint16_t holder, Held held)
    {
        if (kernel().mutex_active == nullptr or ranges == nullptr)
        {
            return;
        }
        size_t const g = arch_aspace_granule();
        for (size_t i = 0; i < VirtualRanges::capacity(); i++)
        {
            VirtualRange const* const e = ranges->at(i);
            if (e != nullptr and (e->flags & VR_WINDOW) != 0 and e->holder == holder)
            {
                mutex_reach_lost(space, e->base, static_cast<size_t>(e->pages) * g, held);
            }
        }
    }
#endif
}
