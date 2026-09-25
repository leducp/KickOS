// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The built-in FIFO + round-robin scheduling policy. This TU owns the ready structure
// (per-priority intrusive FIFO lists plus a priority bitmap) and the RR slice, reached
// only through the SchedPolicy hooks. A FIFO thread has quantum_ns == 0 and never arms
// a slice.

#include <kickos/sched.h>
#include <kickos/instance.h>
#include <kickos/time.h>
#include <kickos/config.h>
#include <kickos/debug.h>

namespace kickos
{
    namespace
    {
        int top_prio(uint32_t bm)
        {
            if (bm == 0)
            {
                return -1;
            }
            return 31 - __builtin_clz(bm);
        }

        // Read from the thread, never the calling core: this is what lets a core remove a
        // thread another core readied.
        inline uint32_t rq_core(Thread const* t)
        {
#if KICKOS_KERNEL_CORES > 1
            return t->queue_core;
#else
            (void)t;
            return 0;
#endif
        }

        // Above one core, `prio` is a field a peer may write under the lock while the thread
        // is linked here, so the key used must be the one this core seated.
        inline uint32_t rq_key(Thread const* t)
        {
#if KICKOS_KERNEL_CORES > 1
            return t->rq_prio;
#else
            return t->prio;
#endif
        }

#if KICKOS_KERNEL_CORES > 1
        // A started core's cell is its bitmap's top plus one, kept so by every bitmap edit, so a
        // placer never reads a level a span with no pass has already moved. An edit that leaves
        // the top where it was stores nothing. `top_of` is any bitmap whose top bit is the level:
        // a started core's own bitmap is never empty, its running thread being on it.
        inline __attribute__((always_inline)) void publish_top(uint32_t c, uint32_t top_of)
        {
            Kernel& k = kernel();
            if (k.current[c] != nullptr)
            {
                KICKOS_DEBUG_ASSERT(top_of != 0);
                k.sched_out[c].level.store(static_cast<uint8_t>(32 - __builtin_clz(top_of)));
            }
        }
#endif

        void rq_push_back(Thread* t)
        {
            uint32_t const c = rq_core(t);
#if KICKOS_KERNEL_CORES > 1
            uint32_t const key = rq_key(t);
            uint32_t const bm = kernel().ready_bitmap[c];
            kernel().ready[c][key].push_back(&t->link);
            kernel().ready_bitmap[c] = bm | (1u << key);
            // The top rises to `key` exactly when no bit at or above it was set.
            if ((bm >> key) == 0)
            {
                publish_top(c, 1u << key);
            }
#else
            kernel().ready[c][rq_key(t)].push_back(&t->link);
            kernel().ready_bitmap[c] |= (1u << rq_key(t));
#endif
        }

        void rq_remove(Thread* t)
        {
            uint32_t const c = rq_core(t);
            List& l = kernel().ready[c][rq_key(t)];
            l.unlink(&t->link);
            if (l.empty())
            {
#if KICKOS_KERNEL_CORES > 1
                uint32_t const key = rq_key(t);
                uint32_t const bm = kernel().ready_bitmap[c] & ~(1u << key);
                kernel().ready_bitmap[c] = bm;
                // The top falls exactly when `key` was it.
                if ((bm >> key) == 0)
                {
                    publish_top(c, bm);
                }
#else
                kernel().ready_bitmap[c] &= ~(1u << rq_key(t));
#endif
            }
        }

        void rq_rotate(Thread* t)
        {
            List& l = kernel().ready[rq_core(t)][rq_key(t)];
            if (l.head == l.tail)
            {
                return;
            }
            l.unlink(&t->link);
            l.push_back(&t->link);
        }

        void arm_slice(Thread* t)
        {
            if (t->policy == Policy::RR and t->quantum_ns > 0)
            {
                // Clamp to the timer's min delta: a deadline already in the past would
                // floor to now+min-delta on every tick and storm interrupts.
                uint64_t q = t->quantum_ns;
                if (q < KICKOS_TIMER_MIN_DELTA_NS)
                {
                    q = KICKOS_TIMER_MIN_DELTA_NS;
                }
                t->slice_deadline_ns = ktime_now() + q;
            }
            else
            {
                t->slice_deadline_ns = UINT64_MAX;
            }
        }

#if KICKOS_KERNEL_CORES > 1
        // A thread a peer core is running, and a peer core's idle fallback, are reserved to
        // that core.
        bool available_to(Thread const* t, uint32_t core)
        {
            if (t == kernel().current[core])
            {
                // A slain thread must lose the core: a core re-picking its own running
                // victim never takes one. Declining here keeps a victim preempted
                // mid-teardown from being displaced by a claim that already fired.
                if (thread_slay_claim_pending(t))
                {
                    return false;
                }
                // Re-masking a running thread is what stops its own core picking it again;
                // reschedule() switches it away rather than unlinking it.
                return sched_placeable_on(t, core);
            }
            // RUNNING and `current` can disagree across a deferred switch, so a thread still
            // on this core's own queue can read RUNNING while this core runs something else;
            // idle is reachable here the same way, before its core has seated it.
            if (t->state == ThreadState::RUNNING)
            {
                return false;
            }
            if (t->prio == KICKOS_PRIO_IDLE and t != kernel().idle[core])
            {
                return false;
            }
            // A core that reaches the end of the scan takes kernel().idle[core] unconditionally
            // below, and add_idle gives idle exactly this core's bit, so the scan cannot hand
            // idle to a peer either.
            return sched_placeable_on(t, core);
        }
#endif

        Thread* policy_pick_next()
        {
#if KICKOS_KERNEL_CORES > 1
            uint32_t const core = kickos_kernel_core();
            uint32_t bm = kernel().ready_bitmap[core];
            int p = top_prio(bm);
            while (p >= 0)
            {
                for (ListNode* n = kernel().ready[core][p].head; n != nullptr; n = n->next)
                {
                    Thread* const t = thread_of(n);
                    if (available_to(t, core))
                    {
                        return t;
                    }
                }
                bm &= ~(1u << static_cast<uint32_t>(p));
                p = top_prio(bm);
            }
            return kernel().idle[core];
#else
            uint32_t const core = kickos_kernel_core();
            int p = top_prio(kernel().ready_bitmap[core]);
            if (p < 0)
            {
                return kernel().idle[core];
            }
            return thread_of(kernel().ready[core][p].head);
#endif
        }

        void policy_on_ready(Thread* t)
        {
            rq_push_back(t);
        }

        void policy_on_remove(Thread* t)
        {
            rq_remove(t);
            // A park or an exit forfeits the slice remainder: the deadline is absolute, so a
            // park shorter than the quantum would otherwise resume on a spent slice. READY or
            // RUNNING here is a re-seat (priority change or cross-core move), which re-adds at
            // once and keeps the deadline.
            if (t->state != ThreadState::READY and t->state != ThreadState::RUNNING)
            {
                t->slice_deadline_ns = UINT64_MAX;
            }
        }

        void policy_on_yield(Thread* t)
        {
            rq_rotate(t);
        }

        void policy_on_slice_expire(Thread* t)
        {
            rq_rotate(t);
            // Arming here is only correct when t keeps the CPU: the deadline is absolute, so
            // arming a thread that is about to rotate behind a peer leaves it a spent sliver by
            // the time it runs again, and that sliver keeps shrinking the pair's slices. With no
            // peer, reschedule() keeps t running with no on_switch_in call to arm it instead.
            if (policy_pick_next() == t)
            {
                arm_slice(t);
                return;
            }
            t->slice_deadline_ns = UINT64_MAX; // the next switch-in arms a full slice
        }

        void policy_on_switch_in(Thread* t)
        {
            // A slice deadline set before the switch must survive it: re-arming here would refund
            // the whole quantum on every resume, starving an equal-priority peer across repeated
            // preemptions. One that passed while t was preempted is spent, and expires as soon as
            // the timer can take it. Zero is a fresh TCB's deadline and arms like none.
            if (t->policy == Policy::RR and t->quantum_ns > 0
                and t->slice_deadline_ns != UINT64_MAX and t->slice_deadline_ns != 0)
            {
                uint64_t const soonest = ktime_now() + KICKOS_TIMER_MIN_DELTA_NS;
                if (t->slice_deadline_ns < soonest)
                {
                    t->slice_deadline_ns = soonest;
                }
                return;
            }
            arm_slice(t);
        }

        uint64_t policy_next_timed_event(Thread const* t)
        {
            if (t == nullptr or t->policy != Policy::RR or t->quantum_ns == 0)
            {
                return UINT64_MAX;
            }
            return t->slice_deadline_ns;
        }

#if KICKOS_KERNEL_CORES > 1
        // A core that has not started takes no doorbell it can act on: its own start picks
        // what it already holds.
        inline bool started(uint32_t core)
        {
            return kernel().sched_out[core].level.load() != 0;
        }

        // The core has seated its first thread, and from here its cell follows its bitmap.
        void policy_on_start(uint32_t core)
        {
            publish_top(core, kernel().ready_bitmap[core]);
        }

        // The priority this core's next pass seats if nothing arrives, `t` excluded, or -1. Read
        // only by the core that holds `t`: a peer's level is its cell (sched_effective_level).
        int level(uint32_t core, Thread const* t)
        {
            uint32_t bm = kernel().ready_bitmap[core];
            if (t != nullptr and t->queue_core == core)
            {
                List const& l = kernel().ready[core][rq_key(t)];
                if (l.head == &t->link and l.tail == &t->link)
                {
                    bm &= ~(1u << rq_key(t));
                }
            }
            return top_prio(bm);
        }

        // Placement invariant: a READY thread never waits behind equal or higher priority while
        // a started core in its mask has a strictly lower level, including equal priority when
        // the mask is wide (a same-core handoff is had by pinning instead). Of the cores
        // strictly below, the lowest level takes it, ties going to the lowest index.
        SchedPlacement policy_place(Thread const* t, uint32_t home)
        {
            int const prio = t->prio;
            if (started(home) and sched_placeable_on(t, home))
            {
                int lvl = 0;
                if (home == t->queue_core)
                {
                    lvl = level(home, t);
                }
                else
                {
                    lvl = sched_effective_level(home);
                }
                if (lvl < prio)
                {
                    return {home, true};
                }
            }
            uint32_t best = home;
            int best_lvl = prio;
            for (uint32_t c = 0; c < KICKOS_KERNEL_CORES; c++)
            {
                if (c == home or not sched_placeable_on(t, c))
                {
                    continue;
                }
                // What is on its way to a core only raises it, so a cell already at the best
                // cannot beat it, and a zero cell is a core not started.
                int const cell = static_cast<int>(kernel().sched_out[c].level.load()) - 1;
                if (cell < 0 or cell >= best_lvl)
                {
                    continue;
                }
                int const lvl = sched_effective_level(c);
                if (lvl < best_lvl)
                {
                    best = c;
                    best_lvl = lvl;
                }
            }
            return {best, best != home};
        }

        // The same invariant seen from a core whose level fell: the first thread, from the top,
        // that waits on `holder` behind equal or higher priority and ranks strictly above
        // `target`. A RUNNING read is refused, not asserted: it can be a current across a
        // deferred switch. A pending slay claim stays, since its core owes it the pass.
        Thread* policy_push_candidate(uint32_t holder, uint32_t target)
        {
            int const floor = sched_effective_level(target);
            uint32_t bm = kernel().ready_bitmap[holder];
            if (floor >= 0)
            {
                bm &= ~((2u << static_cast<uint32_t>(floor)) - 1u);
            }
            int p = top_prio(bm);
            while (p > KICKOS_PRIO_IDLE)
            {
                for (ListNode* n = kernel().ready[holder][p].head; n != nullptr; n = n->next)
                {
                    Thread* const t = thread_of(n);
                    if (t == kernel().current[holder] or t->state != ThreadState::READY
                        or thread_slay_claim_pending(t) or not sched_placeable_on(t, target))
                    {
                        continue;
                    }
                    if (level(holder, t) >= p)
                    {
                        return t;
                    }
                }
                bm &= ~(1u << static_cast<uint32_t>(p));
                p = top_prio(bm);
            }
            return nullptr;
        }

        // Resumes where the last answer left `walk`, so one pass visits each READY thread in the
        // band once: O(threads in the band + priority levels).
        Thread* policy_declined(uint32_t core, int above, SchedWalk* walk)
        {
            while (true)
            {
                ListNode* n = nullptr;
                int p = KICKOS_PRIO_IDLE;
                if (walk->next != nullptr)
                {
                    n = &walk->next->link;
                    p = static_cast<int>(rq_key(walk->next));
                }
                else
                {
                    if (walk->below <= above + 1)
                    {
                        return nullptr;
                    }
                    uint32_t bm = kernel().ready_bitmap[core];
                    if (walk->below < 32)
                    {
                        bm &= (1u << static_cast<uint32_t>(walk->below)) - 1u;
                    }
                    if (above >= 0)
                    {
                        bm &= ~((2u << static_cast<uint32_t>(above)) - 1u);
                    }
                    p = top_prio(bm);
                    if (p <= KICKOS_PRIO_IDLE)
                    {
                        return nullptr;
                    }
                    n = kernel().ready[core][p].head;
                }
                for (; n != nullptr; n = n->next)
                {
                    Thread* const t = thread_of(n);
                    if (t != kernel().current[core] and t->state == ThreadState::READY
                        and not thread_slay_claim_pending(t) and (t->affinity & ~(1u << core)) != 0)
                    {
                        walk->next = nullptr;
                        if (n->next != nullptr)
                        {
                            walk->next = thread_of(n->next);
                        }
                        else
                        {
                            walk->below = p;
                        }
                        return t;
                    }
                }
                walk->next = nullptr;
                walk->below = p;
            }
        }
#endif

        constinit SchedPolicy const g_fifo_rr = {
            policy_pick_next,
            policy_on_ready,
            policy_on_remove,
            policy_on_yield,
            policy_on_slice_expire,
            policy_on_switch_in,
            policy_next_timed_event,
#if KICKOS_KERNEL_CORES > 1
            policy_place,
            policy_push_candidate,
            policy_declined,
            policy_on_start,
#endif
        };
    }

    namespace sched
    {
        SchedPolicy const* default_policy()
        {
            return &g_fifo_rr;
        }
    }
}
