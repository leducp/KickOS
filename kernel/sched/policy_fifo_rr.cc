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
        // Highest set priority (find-first-set from the top), or -1 if the ready
        // set is empty. Bit 0 (idle) is set once idle exists, so it rarely is.
        int highest_prio()
        {
            uint32_t bm = kernel().ready_bitmap;
            if (bm == 0)
            {
                return -1;
            }
            return 31 - __builtin_clz(bm);
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

        Thread* policy_pick_next()
        {
            int p = highest_prio();
            if (p < 0)
            {
                return kernel().idle;
            }
            return thread_of(kernel().ready[p].head);
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
