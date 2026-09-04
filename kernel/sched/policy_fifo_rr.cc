// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The built-in FIFO + round-robin scheduling policy. This TU owns the ready structure
// (per-priority intrusive FIFO lists plus a priority bitmap) and the RR slice; the core
// reaches all of it only through the SchedPolicy hooks, and no other TU may touch the
// ready state in the Kernel struct. A FIFO thread has quantum_ns == 0 and therefore never
// arms a slice.

#include <kickos/sched.h>
#include <kickos/instance.h>
#include <kickos/time.h>
#include <kickos/config.h>

namespace kickos
{
    namespace
    {
        // Highest set priority in `bm`, or -1 when it is empty.
        int top_prio(uint32_t bm)
        {
            if (bm == 0)
            {
                return -1;
            }
            return 31 - __builtin_clz(bm);
        }

        // Highest set priority of the whole ready set.
        int highest_prio()
        {
            return top_prio(kernel().ready_bitmap);
        }

        void rq_push_back(Thread* t)
        {
            kernel().ready[t->prio].push_back(&t->link);
            kernel().ready_bitmap |= (1u << t->prio);
        }

        void rq_remove(Thread* t)
        {
            List& l = kernel().ready[t->prio];
            l.unlink(&t->link);
            if (l.empty())
            {
                kernel().ready_bitmap &= ~(1u << t->prio);
            }
        }

        void rq_rotate(Thread* t)
        {
            // Move t to the back of its priority list (no-op if it's the only one).
            List& l = kernel().ready[t->prio];
            if (l.head == l.tail)
            {
                return;
            }
            l.unlink(&t->link);
            l.push_back(&t->link);
        }

        // Arm (or clear) the RR slice deadline for a thread that is about to run.
        void arm_slice(Thread* t)
        {
            if (t->policy == Policy::RR and t->quantum_ns > 0)
            {
                // Cannot slice finer than the one-shot timer resolution: clamp so
                // the deadline is never immediately in the past (which would floor
                // to now+min-delta every tick -> interrupt storm). A sub-min-delta
                // quantum (incl. a hostile user value) collapses to the min slice.
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
        // Whether core `core` may take `t`. A thread a peer core is running, and a peer core's
        // idle fallback, are reserved to that core.
        bool available_to(Thread const* t, uint32_t core)
        {
            if (t == kernel().current[core])
            {
                // THE PLACEMENT CHECK AND NOT A BARE TRUE, WHICH IS THE WHOLE OF MIGRATION:
                // re-masking a running thread is what stops its own core picking it again, so
                // reschedule() switches away instead of the thread being yanked off.
                return sched_placeable_on(t, core);
            }
            if (t->state == ThreadState::RUNNING)
            {
                return false;
            }
            if (t->prio == KICKOS_PRIO_IDLE and t != kernel().idle[core])
            {
                return false;
            }
            // Idle needs no exemption from isolation: a core that reaches the end of the scan
            // takes kernel().idle[core] unconditionally below, and add_idle gives idle exactly
            // this core's bit so the scan cannot hand it to a peer either.
            return sched_placeable_on(t, core);
        }
#endif

        Thread* policy_pick_next()
        {
#if KICKOS_KERNEL_CORES > 1
            uint32_t const core = kickos_kernel_core();
            uint32_t bm = kernel().ready_bitmap;
            int p = highest_prio();
            while (p >= 0)
            {
                for (ListNode* n = kernel().ready[p].head; n != nullptr; n = n->next)
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
            int p = highest_prio();
            if (p < 0)
            {
                return kernel().idle[kickos_kernel_core()];
            }
            return thread_of(kernel().ready[p].head);
#endif
        }

        void policy_on_ready(Thread* t)
        {
            rq_push_back(t);
        }

        void policy_on_remove(Thread* t)
        {
            rq_remove(t);
            // A park or an exit forfeits the slice remainder. The deadline is absolute, so
            // a park shorter than the quantum resumes on whatever wall time was left and
            // is rotated away microseconds after waking. Every parking path sets `state`
            // before detaching, so READY/RUNNING here means the sole re-seating caller
            // (sched::set_prio), which re-adds at once and must keep the deadline: a
            // boost/unboost pair would otherwise refund the whole quantum.
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
            // Arming HERE is only correct when t keeps the CPU: the deadline is absolute,
            // and a thread rotated behind a peer does not run again for a whole quantum,
            // so a now+q written here is already most of the way spent by the time
            // on_switch_in sees it, and on_switch_in then honours it as a remainder and hands t a
            // sliver. The sliver shortens the peer's next slice the same way, so the pair
            // never recovers a full slice. With no peer, reschedule() keeps t running and
            // no on_switch_in follows, so the arm has to happen here or the deadline stays
            // in the past and re-fires every min-delta.
            if (policy_pick_next() == t)
            {
                arm_slice(t);
                return;
            }
            t->slice_deadline_ns = UINT64_MAX; // spent: the next switch-in arms a full one
        }

        void policy_on_switch_in(Thread* t)
        {
            // A slice deadline still in the future MUST survive the switch. Re-arming it
            // here would refund the whole quantum on every resume, so a thread preempted
            // more often than its quantum would never expire a slice and would starve its
            // equal-priority peer for as long as the preemption lasted.
            if (t->policy == Policy::RR and t->quantum_ns > 0
                and t->slice_deadline_ns != UINT64_MAX
                and t->slice_deadline_ns > ktime_now())
            {
                return;
            }
            arm_slice(t);
        }

        uint64_t policy_next_timed_event()
        {
            Thread* c = kernel().current[kickos_kernel_core()];
            if (c == nullptr or c->policy != Policy::RR or c->quantum_ns == 0)
            {
                return UINT64_MAX;
            }
            return c->slice_deadline_ns;
        }

        constinit SchedPolicy const g_fifo_rr = {
            policy_pick_next,
            policy_on_ready,
            policy_on_remove,
            policy_on_yield,
            policy_on_slice_expire,
            policy_on_switch_in,
            policy_next_timed_event,
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
