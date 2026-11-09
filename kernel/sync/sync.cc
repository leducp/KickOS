// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/sync.h>
#include <kickos/bench.h>
#include <kickos/sched.h>
#include <kickos/smptrace.h>
#include <kickos/cap.h>
#include <kickos/domain.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/irqlock.h>
#include <kickos/klock.h>
#include <kickos/task.h>
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

    uint32_t wq_park(List& q, WaitKind kind, void* obj, Held held)
    {
        Thread* c = sched::current();
        uint32_t const epoch = park_queueless(c, kind, obj, held);
        c->wait_queue = &q;
        q.push_back(&c->link);
        // BEFORE the reschedule, which on a stall never returns.
        KOS_TRACE(::kickos::KOS_TR_PARK, KOS_TRACE_ID(c), KOS_TRACE_ID(&q));
        return epoch;
    }

    uint32_t wq_block(List& q, WaitKind kind, void* obj, Thread* woken, Held held)
    {
        uint32_t const epoch = wq_park(q, kind, obj, held);
        sched::reschedule(woken, held);
        return epoch;
    }

    // sched::set_prio is the sole writer of an effective priority. Inheritance stops at a plain
    // word: a thread parked on one answers nullptr from wait_mutex(), so the chain walk stops
    // there.
    namespace
    {
        void held_push(Thread* owner, WordRecord* m)
        {
            m->next_held = owner->held_list;
            owner->held_list = m;
        }

        void held_remove(Thread* owner, WordRecord* m)
        {
            WordRecord** pp = &owner->held_list;
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
        void transfer_to(WordRecord* m, Thread* w, intptr_t status)
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

    int endpoint_slot_claim(Task const* charged, Endpoint** out)
    {
        int const i = kernel().endpoints.alloc(charged);
        Endpoint* const ep = kernel().endpoints.at(i); // null at either refusal
        if (ep == nullptr)
        {
            return i;
        }
        *ep = Endpoint{};
        *out = ep;
        return i;
    }

    int endpoint_claim_created(Task const* charged, Endpoint** out)
    {
        int const i = endpoint_slot_claim(charged, out);
        if (i >= 0)
        {
            (*out)->recv_holders = 1;
            (*out)->handout_holders = 1;
        }
        return i;
    }

    namespace
    {
        // The highest of `p` and the priorities of the waiters on `l` that wait on the death of the
        // task whose run list `t` is on. *first caches that run list's first node, found on the
        // first waiter that outranks `p`.
        uint8_t waiters_lend(Thread const* t, uint8_t p, HeadList const& l, ListNode const** first)
        {
            for (ListNode* n = l.head; n != nullptr; n = n->next)
            {
                Thread* const w = thread_of(n);
                if (w->prio <= p)
                {
                    continue;
                }
                if (*first == nullptr)
                {
                    *first = task_run_first(t);
                }
                if (task_runs(w->wait_task_target()) == *first)
                {
                    p = w->prio;
                }
            }
            return p;
        }
    }

    // The single effective-priority funnel. Runs under IrqLock on every mutex unlock,
    // reply and close, so NO term added here may walk the capability table.
    uint8_t thread_effective_prio(Thread* t)
    {
        uint8_t p = t->base_prio;
        for (WordRecord* h = t->held_list; h != nullptr; h = h->next_held)
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
        // O(W + t's place on its run list), interrupt-masked, W the death waiters, at most the
        // thread slots; one empty-list test each with none.
        ListNode const* first = nullptr;
        p = waiters_lend(t, p, kernel().death_waiters, &first);
        p = waiters_lend(t, p, kernel().death_dirty, &first);
        return p;
    }

    namespace
    {
        Thread* pi_next(Thread const* o)
        {
            WordRecord const* const m = o->wait_mutex();
            if (m == nullptr)
            {
                return nullptr;
            }
            return m->owner;
        }

        // Death waiter `w`'s priority moved: its task's threads owe it a recompute.
        void death_dirty_mark(Thread* w)
        {
            Kernel& k = kernel();
            if (list_contains(k.death_dirty.head, &w->link))
            {
                return;
            }
            k.death_waiters.unlink(&w->link);
            k.death_dirty.push(&w->link);
        }

        // Stops at the first thread already at or above `p`, what it waits on having it too, and
        // at a death waiter, whose task pi_drain_deaths visits. The depth bound only stops the
        // walk on a foreign cycle.
        void pi_raise_chain(Thread* o, uint8_t p, Held held)
        {
            for (int depth = 0; o != nullptr and depth <= kernel().threads.count(); depth++)
            {
                if (o->prio >= p)
                {
                    return;
                }
                sched::set_prio(o, p, held);
                if (o->wait_task_target() != nullptr)
                {
                    death_dirty_mark(o);
                    return;
                }
                o = pi_next(o);
            }
        }

        // As pi_raise_chain, a recompute in place of a raise: stops at the first thread the
        // recompute leaves where it was.
        void pi_settle_chain(Thread* o, Held held)
        {
            for (int depth = 0; o != nullptr and depth <= kernel().threads.count(); depth++)
            {
                uint8_t const p = thread_effective_prio(o);
                if (p == o->prio)
                {
                    return;
                }
                sched::set_prio(o, p, held);
                if (o->wait_task_target() != nullptr)
                {
                    death_dirty_mark(o);
                    return;
                }
                o = pi_next(o);
            }
        }

        void pi_settle_runs(Task const* t, Held held)
        {
            for (ListNode* n = task_runs(t); n != nullptr; n = n->next)
            {
                pi_settle_chain(KICKOS_CONTAINER_OF(n, Thread, reclaim), held);
            }
        }

#ifndef KICKOS_PI_DRAIN_POPS
#define KICKOS_PI_DRAIN_POPS (kernel().threads.count() + 1)
#endif

        // Each death waiter whose priority moved has its task's threads recomputed, which can
        // move another. From a consistent state one priority change moves each thread at most
        // once, a lowered value never rising again within the drain nor a raised one falling,
        // so a waiter is dirtied at most once: pops <= W <= N, the thread slots, each recomputing
        // the M threads of a task and their mutex chains, up to N + 1 each. A waited task is
        // ended or stopped before it is lent to, its threads waiting on nothing, so in a state
        // the ABI reaches a drain pops at most the waiter its walk stopped at. KICKOS_PI_DRAIN_POPS
        // (N + 1) is not reached from a consistent state; past it the lends are left as they
        // stand, silent in a release build as the mutex chain's bound is. A test posture lowers
        // it to reach that arm.
        void pi_drain_deaths(Held held)
        {
            Kernel& k = kernel();
            for (int pops = 0; not k.death_dirty.empty(); pops++)
            {
                if (pops >= KICKOS_PI_DRAIN_POPS)
                {
                    KICKOS_DEBUG_ASSERT(KICKOS_PI_DRAIN_POPS < kernel().threads.count() + 1);
                    while (not k.death_dirty.empty())
                    {
                        k.death_waiters.push(k.death_dirty.pop());
                    }
                    return;
                }
                ListNode* const w = k.death_dirty.pop();
                k.death_waiters.push(w);
                pi_settle_runs(thread_of(w)->wait_task_target(), held);
            }
        }
    }

    void pi_raise(Thread* o, uint8_t p, Held held)
    {
        pi_raise_chain(o, p, held);
        pi_drain_deaths(held);
    }

    void pi_settle(Thread* o, Held held)
    {
        pi_settle_chain(o, held);
        pi_drain_deaths(held);
    }

    uint32_t death_park(Thread* c, Task* t, Held held)
    {
        uint32_t const epoch = park_queueless(c, WAIT_TASK_EMPTY, t, held);
        kernel().death_waiters.push(&c->link);
        return epoch;
    }

    void death_wait_lend(Task const* t, Held held)
    {
        pi_settle_runs(t, held);
        pi_drain_deaths(held);
    }

    // Also from the timer interrupt, for a wait that timed out.
    void death_wait_leave(Thread* w, Held held)
    {
        Kernel& k = kernel();
        Task* const t = w->wait_task_target();
        if (not k.death_dirty.unlink_if_present(&w->link))
        {
            k.death_waiters.unlink(&w->link);
        }
        if (k.death_leave_batch)
        {
            task_lend_owe(t);
            k.death_leave_owed = true;
            if (w->prio > k.death_leave_top)
            {
                k.death_leave_top = w->prio;
            }
            return;
        }
        death_wait_lend(t, held);
    }

    void death_leave_batch_begin()
    {
        kernel().death_leave_batch = true;
    }

    void death_leave_batch_end(Held held)
    {
        Kernel& k = kernel();
        k.death_leave_batch = false;
        if (not k.death_leave_owed)
        {
            return;
        }
        k.death_leave_owed = false;
        uint8_t const top = k.death_leave_top;
        k.death_leave_top = 0;
        Thread* const c = sched::current();
        uint8_t before = 0;
        if (c != nullptr)
        {
            before = c->prio;
        }
        for (Task& t : kernel().tasks())
        {
            if (task_lend_owed_take(&t))
            {
                pi_settle_runs(&t, held);
            }
        }
        pi_drain_deaths(held);
        // The wakes in between chose against the current's lend: lowered, it owes the pass
        // resched_after_wake would have taken, an exiting thread rotated only by a higher wake.
        if (c == nullptr or c->prio >= before or c->state == ThreadState::EXITED
            or (c->dying and top <= c->prio))
        {
            return;
        }
        sched::reschedule(nullptr, held);
    }

    namespace
    {
        // A PI word's holder id, a plain word's count.
        constexpr uint32_t WORD_VALUE = ~static_cast<uint32_t>(KOS_WORD_WAITERS);

        WordRecord* record_find(arch_phys_addr_t key)
        {
            for (WordRecord* m = kernel().word_active; m != nullptr; m = m->next_active)
            {
                if (m->key == key)
                {
                    return m;
                }
            }
            return nullptr;
        }

        // Never null: each waiter parks on one record. `owner` is a PI record's alone.
        WordRecord* record_take(arch_phys_addr_t key, WordKind kind, Thread* owner)
        {
            Kernel& k = kernel();
            for (WordRecord& m : k.word_records())
            {
                if (m.kind == WordKind::FREE)
                {
                    m.key = key;
                    m.kind = kind;
                    m.owner = owner;
                    m.waiters = List{};
                    m.prev_active = nullptr;
                    m.next_active = k.word_active;
                    if (k.word_active != nullptr)
                    {
                        k.word_active->prev_active = &m;
                    }
                    k.word_active = &m;
                    if (kind == WordKind::PI)
                    {
                        held_push(owner, &m);
                    }
                    return &m;
                }
            }
            KICKOS_UNREACHABLE(::kickos::diag::kWordNoRecord);
        }

        // `m` has no waiter left and, a PI record, is off its owner's held list.
        void record_free(WordRecord* m)
        {
            if (m->prev_active == nullptr)
            {
                kernel().word_active = m->next_active;
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
            m->kind = WordKind::FREE;
        }

        // The live thread the id names, if it could have written the word: anything else is a
        // dead holder. An id cannot name idle, whose TCB sits outside the pool.
        Thread* owner_resolve(uint32_t id, WordRef const& w)
        {
            uint32_t const slot = id & LOCK_ID_SLOT_MASK;
            if (slot == 0 or slot > static_cast<uint32_t>(kernel().threads.count()))
            {
                return nullptr;
            }
            Thread* const t = &kernel().threads.slots()[slot - 1u];
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
        Attempt lock_attempt(IrqLock& lock, Thread* c, uintptr_t va, WaitLimit limit, int* rc,
                             uint32_t* epoch)
        {
            // Ahead of the donation walk below, whose boosts an exit taken at the park would
            // leave seated on this caller's behalf with the caller gone.
            ParkToken const ask = park_cancel_pending(c);
            if (ask.cancelled())
            {
                sched::exit_current(KOS_EXIT_CANCELLED, sched::EXIT_RETURN, &lock);
            }
            WordRef w;
            WordRecord* m = nullptr;
            Thread* t = nullptr;
            // The hold of the word ends before the park.
            {
                WordAccess word(c, va, &w);
                *rc = word.status();
                if (*rc != 0)
                {
                    return Attempt::ANSWERED;
                }
                uint32_t const v = word.load();
                m = record_find(w.key);
                if (m != nullptr and m->kind != WordKind::PI)
                {
                    *rc = -KOS_EINVAL;
                    return Attempt::ANSWERED;
                }
                if (m != nullptr)
                {
                    // The record is the authority: a word naming anyone else broke the protocol.
                    if ((v & WORD_VALUE) != m->owner->lock_id)
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
                    if ((v & KOS_WORD_WAITERS) == 0
                        and not word.cas(&expected, v | KOS_WORD_WAITERS))
                    {
                        return Attempt::RETRY;
                    }
                    t = m->owner;
                }
                else
                {
                    uint32_t const id = v & WORD_VALUE;
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
                // Before any deadline: ktime_limit_arm treats only KOS_TIMEOUT_NONE as none,
                // and would park a try until the next expiry.
                if (ktime_limit_passed(limit))
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
                        if (depth > kernel().threads.count())
                        {
                            break;
                        }
                    }
                }
                uint32_t expected = v;
                if (m == nullptr and (v & KOS_WORD_WAITERS) == 0
                    and not word.cas(&expected, v | KOS_WORD_WAITERS))
                {
                    return Attempt::RETRY;
                }
            }
            if (m == nullptr)
            {
                m = record_take(w.key, WordKind::PI, t);
            }
            c->wait_va = va;
            // WAIT_MUTEX is what puts c on the chain walk below for the next blocker. A waker
            // transfers the record and writes wait_result.
            *epoch = wq_park(ask)(m->waiters, WAIT_MUTEX, m, lock);
            // Pass 2, the chain now known acyclic, with c on the waiters its recomputes read.
            pi_raise(t, c->prio, lock);
            ktime_limit_arm(c, limit, lock);
            sched::reschedule(nullptr, lock);
            return Attempt::PARKED;
        }

        Attempt unlock_attempt(IrqLock& lock, Thread* c, uintptr_t va, int* rc)
        {
            WordRef w;
            WordRecord* m = nullptr;
            {
                WordAccess word(c, va, &w);
                *rc = word.status();
                if (*rc != 0)
                {
                    return Attempt::ANSWERED;
                }
                uint32_t const v = word.load();
                m = record_find(w.key);
                if (m != nullptr and m->kind != WordKind::PI)
                {
                    *rc = -KOS_EINVAL;
                    return Attempt::ANSWERED;
                }
                if (m == nullptr)
                {
                    if ((v & WORD_VALUE) != c->lock_id)
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
                    handed = handed | KOS_WORD_WAITERS;
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

        uint32_t waiters_of(WordRecord const* m)
        {
            uint32_t n = 0;
            if (m == nullptr)
            {
                return 0;
            }
            for (ListNode const* l = m->waiters.head; l != nullptr; l = l->next)
            {
                n++;
            }
            return n;
        }

        struct WakePlan
        {
            uint32_t woken;
            uint32_t handed;
            uint32_t banked;
            bool keep_bit;
        };

        WakePlan wake_plan(uint32_t parked, uint32_t n, uint32_t units)
        {
            WakePlan p{n, units, 0u, false};
            if (parked < p.woken)
            {
                p.woken = parked;
            }
            if (p.woken < p.handed)
            {
                p.handed = p.woken;
            }
            // Non-zero only where the record empties: a unit banked beside a parked waiter is
            // one a running taker could take first.
            p.banked = units - p.handed;
            p.keep_bit = p.woken < parked;
            return p;
        }

        // The word a wake commits over `v`; false where the banked units carry out of the count.
        bool word_after_wake(uint32_t v, WakePlan const& p, uint32_t* next)
        {
            uint32_t const count = v & WORD_VALUE;
            if (p.banked > WORD_VALUE - count)
            {
                return false;
            }
            *next = count + p.banked;
            if (p.keep_bit)
            {
                *next = *next | KOS_WORD_WAITERS;
            }
            return true;
        }

        // Every waiter popped is READY, its deadline dropped, before the one pass that may switch
        // away: one still BLOCKED off its queue across that switch has no wait edge for an expiry
        // or a kill to unwind, and a slain waker would never wake it.
        void wake_popped(WordRecord* m, WakePlan const& p, Held held)
        {
            Thread* top = nullptr;
            for (uint32_t i = 0; i < p.woken; i++)
            {
                Thread* const t = wq_pop_highest(m->waiters);
                t->wait_result = 0;
                if (i < p.handed)
                {
                    t->wait_result = 1;
                }
                if (not sched::wake_no_resched(t, held))
                {
                    continue;
                }
                // Popped highest first, so the first readied is the one the pass weighs.
                if (top == nullptr)
                {
                    top = t;
                    continue;
                }
#if KICKOS_KERNEL_CORES > 1
                sched::place_ready(t, held);
#endif
            }
            if (m != nullptr and m->waiters.empty())
            {
                record_free(m);
            }
            if (top != nullptr)
            {
                sched::resched_after_wake(top, held);
            }
        }

        // Every decision on the word is made on the value the compare-and-swap commits over, and
        // nobody is popped before it lands: a user fast path on another core moves the count
        // between any load and any store.
        Attempt wake_attempt(IrqLock& lock, Thread* c, uintptr_t va, uint32_t n, uint32_t units,
                             int* rc)
        {
            WordRef w;
            WordRecord* m = nullptr;
            WakePlan p{};
            {
                WordAccess word(c, va, &w);
                *rc = word.status();
                if (*rc != 0)
                {
                    return Attempt::ANSWERED;
                }
                m = record_find(w.key);
                if (m != nullptr and m->kind != WordKind::PLAIN)
                {
                    *rc = -KOS_EINVAL;
                    return Attempt::ANSWERED;
                }
                p = wake_plan(waiters_of(m), n, units);
                uint32_t const v = word.load();
                uint32_t next = 0;
                if (not word_after_wake(v, p, &next))
                {
                    *rc = -KOS_EOVERFLOW;
                    return Attempt::ANSWERED;
                }
                uint32_t expected = v;
                if (not word.cas(&expected, next))
                {
                    return Attempt::RETRY;
                }
            }
            *rc = static_cast<int>(p.woken);
            wake_popped(m, p, lock);
            return Attempt::ANSWERED;
        }
    }

    int mutex_lock(uintptr_t va, WaitLimit limit)
    {
        if (not limit.valid())
        {
            return -KOS_EINVAL;
        }
        Thread* const c = sched::current();
        uint32_t epoch = 0;
        while (true)
        {
            int rc = 0;
            Attempt got = Attempt::RETRY;
            {
                IrqLock lock;
                got = lock_attempt(lock, c, va, limit, &rc, &epoch);
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

    int word_wait(uintptr_t va, uint32_t expected, WaitLimit limit)
    {
        if (not limit.valid())
        {
            return -KOS_EINVAL;
        }
        Thread* const c = sched::current();
        uint32_t epoch = 0;
        {
            IrqLock lock;
            ParkToken const ask = park_cancel_pending(c);
            if (ask.cancelled())
            {
                sched::exit_current(KOS_EXIT_CANCELLED, sched::EXIT_RETURN, &lock);
            }
            WordRef w;
            WordRecord* m = nullptr;
            {
                WordAccess word(c, va, &w);
                if (word.status() != 0)
                {
                    return word.status();
                }
                m = record_find(w.key);
                if (m != nullptr and m->kind != WordKind::PLAIN)
                {
                    return -KOS_EINVAL;
                }
                if (word.load() != expected)
                {
                    return -KOS_EAGAIN;
                }
            }
            // Before any deadline: ktime_limit_arm treats only KOS_TIMEOUT_NONE as none.
            if (ktime_limit_passed(limit))
            {
                return -KOS_ETIMEDOUT;
            }
            if (m == nullptr)
            {
                m = record_take(w.key, WordKind::PLAIN, nullptr);
            }
            c->wait_va = va;
            ktime_limit_arm(c, limit, lock);
            epoch = wq_block(ask)(m->waiters, WAIT_WORD, m, nullptr, lock);
        }
        wq_confirm_resume(c, epoch);
        return static_cast<int>(c->wait_result);
    }

    int word_wake(uintptr_t va, uint32_t n, uint32_t units)
    {
        if (units > n)
        {
            return -KOS_EINVAL;
        }
        Thread* const c = sched::current();
        while (true)
        {
            int rc = 0;
            Attempt got = Attempt::RETRY;
            {
                IrqLock lock;
                got = wake_attempt(lock, c, va, n, units, &rc);
            }
            if (got == Attempt::ANSWERED)
            {
                return rc;
            }
        }
    }

    void word_waiter_left(WordRecord* m)
    {
        if (m->waiters.empty())
        {
            if (m->kind == WordKind::PI)
            {
                held_remove(m->owner, m);
            }
            record_free(m);
        }
    }

    void mutex_owner_exit(Thread* dying, Held held)
    {
        // `dying` gets NO recompute: it stays boosted for the remainder of its own teardown, a
        // bounded inversion since the sweep is chunked. The waiter is woken with
        // MUTEX_OWNER_DIED, the protected state may be inconsistent, but never stranded.
#if KICKOS_BENCH_WALKS
        BenchTick const walk_start = bench_cyccnt();
        uint32_t handed_records = 0;
#endif
        while (dying->held_list != nullptr)
        {
#if KICKOS_BENCH_WALKS
            handed_records++;
#endif
            WordRecord* const m = dying->held_list;
            held_remove(dying, m);
            Thread* const next = wq_pop_highest(m->waiters);
            KICKOS_ASSERT(next != nullptr); // a record lives only while it has a waiter
            uint32_t handed = next->lock_id;
            if (not m->waiters.empty())
            {
                handed = handed | KOS_WORD_WAITERS;
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
#if KICKOS_BENCH_WALKS
        if (handed_records != 0)
        {
            bench_walk_add(BD_OWNER_EXIT, bench_cyccnt() - walk_start, handed_records);
        }
#endif
    }

#if KICKOS_HAVE_ASPACE
    namespace
    {
        // The pass is weighed on the highest thread woken: a dying caller declines one for a
        // thread it outranks.
        void reach_lost_woke(Thread* t, Thread** top, [[maybe_unused]] Held held)
        {
            if (*top == nullptr)
            {
                *top = t;
                return;
            }
            Thread* placed = t;
            if (t->prio > (*top)->prio)
            {
                placed = *top;
                *top = t;
            }
#if KICKOS_KERNEL_CORES > 1
            sched::place_ready(placed, held);
#else
            (void)placed;
#endif
        }

        // Readies and never switches: a switch here lets other threads unpark, free and reuse
        // what the walk's saved pointers name before it resumes on them.
        void reach_lost_scan(struct arch_aspace* space, uintptr_t base, size_t bytes,
                             Thread** top, Held held)
        {
#if KICKOS_BENCH_WALKS
            BenchTick const scan_start = bench_cyccnt();
            uint32_t ended_waits = 0;
#endif
            WordRecord* m = kernel().word_active;
            while (m != nullptr)
            {
                // Saved first: the abort frees a record its last waiter leaves.
                WordRecord* const next_record = m->next_active;
                ListNode* n = m->waiters.head;
                while (n != nullptr)
                {
                    ListNode* const after = n->next;
                    Thread* const t = thread_of(n);
                    if (domain_space(thread_domain(t)) == space and t->wait_va >= base
                        and t->wait_va - base < bytes)
                    {
                        if (thread_abort_park_no_resched(t, -KOS_EFAULT, held))
                        {
                            reach_lost_woke(t, top, held);
                        }
#if KICKOS_BENCH_WALKS
                        ended_waits++;
#endif
                    }
                    n = after;
                }
                m = next_record;
            }
#if KICKOS_BENCH_WALKS
            if (ended_waits != 0)
            {
                bench_walk_add(BD_REACH_LOST, bench_cyccnt() - scan_start, ended_waits);
            }
#endif
        }
    }

    void word_reach_lost(struct arch_aspace* space, uintptr_t base, size_t bytes, Held held)
    {
        Thread* top = nullptr;
        reach_lost_scan(space, base, bytes, &top, held);
        if (top != nullptr)
        {
            sched::resched_after_wake(top, held);
        }
    }

    void word_windows_lost(struct arch_aspace* space, VirtualRanges const* ranges,
                           uint16_t holder, Held held)
    {
        if (kernel().word_active == nullptr or ranges == nullptr)
        {
            return;
        }
        size_t const g = arch_aspace_granule();
        Thread* top = nullptr;
        for (size_t i = 0; i < VirtualRanges::capacity(); i++)
        {
            VirtualRange const* const e = ranges->at(i);
            if (e != nullptr and (e->flags & VR_WINDOW) != 0 and e->holder == holder)
            {
                reach_lost_scan(space, e->base, static_cast<size_t>(e->pages) * g, &top, held);
            }
        }
        if (top != nullptr)
        {
            sched::resched_after_wake(top, held);
        }
    }
#endif
}
