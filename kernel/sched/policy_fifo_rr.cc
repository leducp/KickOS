// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The built-in FIFO + round-robin scheduling policy (RTEMS-style). This TU owns
// the ready structure -- per-priority intrusive FIFO lists + a priority bitmap
// (find-first-set for the highest non-empty) -- and the RR slice; the core
// reaches all of it only through the SchedPolicy hooks. RR is FIFO plus
// on_slice_expire rotation; a FIFO thread has quantum_ns == 0, so it never arms
// a slice (no "FIFO slice"). The ready state lives in the Kernel struct
// (instance-scoped); only this TU touches it.

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
        }

        void policy_on_yield(Thread* t)
        {
            rq_rotate(t);
        }

        void policy_on_slice_expire(Thread* t)
        {
            // Rotate to the equal-priority peer, then re-grant a fresh quantum: if
            // there is no peer, reschedule() keeps t running and its deadline must
            // not stay in the past (which would re-arm every min-delta -- a storm).
            rq_rotate(t);
            arm_slice(t);
        }

        void policy_on_switch_in(Thread* t)
        {
            arm_slice(t);
        }

        uint64_t policy_next_timed_event()
        {
            Thread* c = kernel().current;
            if (c == nullptr or c->policy != Policy::RR or c->quantum_ns == 0)
            {
                return UINT64_MAX;
            }
            return c->slice_deadline_ns;
        }

        SchedPolicy const g_fifo_rr = {
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
