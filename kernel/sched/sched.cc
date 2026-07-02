// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Scheduler core: per-priority FIFO ready lists + a priority bitmap, one
// decision point (reschedule), and a pluggable FIFO/RR policy. The running
// thread stays at the front of its priority list; switches always target the
// front of the highest non-empty list, so equal-priority FIFO never preempts
// and RR rotation is just move-to-back of the current thread.

#include <kickos/sched.h>
#include <kickos/kernel.h>
#include <kickos/time.h>
#include <kickos/irqlock.h>

namespace kickos
{
    namespace
    {

        // --- Per-priority ready lists + bitmap -------------------------------------
        struct ReadyList
        {
            Thread* head = nullptr;
            Thread* tail = nullptr;
        };

        ReadyList g_ready[KICKOS_NUM_PRIO];
        uint32_t g_bitmap = 0; // bit p set iff g_ready[p] non-empty

        Thread* g_current = nullptr;
        Thread* g_idle = nullptr;
        unsigned g_live = 0; // non-idle threads not yet EXITED
        arch_context g_boot;

        SchedPolicy const* g_policy = nullptr;

        // Highest set priority (find-first-set from the top). Bit 0 (idle) always set
        // once idle exists, so the queue is never empty.
        inline int highest_prio()
        {
            if (g_bitmap == 0)
            {
                return -1;
            }
            return 31 - __builtin_clz(g_bitmap);
        }

        void rq_push_back(Thread* t)
        {
            ReadyList& l = g_ready[t->prio];
            t->qnext = nullptr;
            t->qprev = l.tail;
            if (l.tail != nullptr)
            {
                l.tail->qnext = t;
            }
            else
            {
                l.head = t;
            }
            l.tail = t;
            g_bitmap |= (1u << t->prio);
        }

        void rq_remove(Thread* t)
        {
            ReadyList& l = g_ready[t->prio];
            if (t->qprev != nullptr)
            {
                t->qprev->qnext = t->qnext;
            }
            else
            {
                l.head = t->qnext;
            }
            if (t->qnext != nullptr)
            {
                t->qnext->qprev = t->qprev;
            }
            else
            {
                l.tail = t->qprev;
            }
            t->qnext = nullptr;
            t->qprev = nullptr;
            if (l.head == nullptr)
            {
                g_bitmap &= ~(1u << t->prio);
            }
        }

        void rq_rotate(Thread* t)
        {
            // Move t to the back of its priority list (no-op if it's the only one).
            ReadyList& l = g_ready[t->prio];
            if (l.head == l.tail)
            {
                return;
            }
            rq_remove(t);
            rq_push_back(t);
        }

        // --- Default FIFO/RR policy ------------------------------------------------
        Thread* policy_pick_next()
        {
            int p = highest_prio();
            if (p < 0)
            {
                return g_idle;
            }
            return g_ready[p].head;
        }
        void policy_on_ready(Thread*)
        {
        }
        void policy_on_remove(Thread*)
        {
        }
        void policy_on_yield(Thread* t)
        {
            rq_rotate(t);
        }
        void policy_on_slice_expire(Thread* t)
        {
            rq_rotate(t);
        }

        SchedPolicy const g_fifo_rr = {
            policy_pick_next,
            policy_on_ready,
            policy_on_remove,
            policy_on_yield,
            policy_on_slice_expire,
        };

        // Arm the RR slice deadline for a thread being switched in.
        void arm_slice(Thread* t)
        {
            if (t->policy == Policy::RR && t->quantum_ns > 0)
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

        // The one place a switch happens. Caller holds the critical section.
        void switch_to(Thread* next)
        {
            Thread* prev = g_current;
            if (prev->state == ThreadState::RUNNING)
            {
                prev->state = ThreadState::READY;
            }
            g_current = next;
            next->state = ThreadState::RUNNING;
            next->switch_count++;
            arm_slice(next);
            // Task-switch hook: reprogram per-task MPU regions in one place.
            arch_mpu_apply(next->regions, next->region_count);
            // Arm the next-event timer for the incoming thread BEFORE we jump: the
            // switching-out thread will not return here until it is resumed, so its
            // deadlines (RR slice) must be programmed now.
            ktime_rearm();
            arch_switch(&prev->ctx, &next->ctx);
        }

    }

    namespace sched
    {

        void init()
        {
            for (int i = 0; i < KICKOS_NUM_PRIO; i++)
            {
                g_ready[i] = ReadyList{};
            }
            g_bitmap = 0;
            g_current = nullptr;
            g_idle = nullptr;
            g_live = 0;
            g_policy = &g_fifo_rr;
        }

        void set_policy(SchedPolicy const* policy)
        {
            g_policy = policy;
        }

        void add(Thread* t)
        {
            IrqLock lock;
            t->state = ThreadState::READY;
            rq_push_back(t);
            g_policy->on_ready(t);
            if (t->prio == KICKOS_PRIO_IDLE)
            {
                g_idle = t;
            }
            else
            {
                g_live++;
            }
        }

        void start()
        {
            IrqLock lock;
            Thread* first = g_policy->pick_next();
            g_current = first;
            first->state = ThreadState::RUNNING;
            arm_slice(first);
            arch_mpu_apply(first->regions, first->region_count);
            ktime_rearm();
            arch_start(&g_boot, &first->ctx);
        }

        void reschedule()
        {
            IrqLock lock;
            Thread* next = g_policy->pick_next();
            if (next == g_current)
            {
                return;
            }
            switch_to(next); // arms the next-event timer for the incoming thread
        }

        void yield()
        {
            IrqLock lock;
            g_policy->on_yield(g_current);
            reschedule();
        }

        void detach_current()
        {
            IrqLock lock;
            // Blocking is only legal from thread context: a voluntary block relies
            // on arch_switch completing synchronously. From ISR context arch_switch
            // would defer and the "blocked" thread would keep running.
            if (arch_in_isr())
            {
                kpanic("kickos: blocking operation from ISR context");
            }
            rq_remove(g_current);
            g_policy->on_remove(g_current);
        }

        void block_current()
        {
            IrqLock lock;
            // Caller has set g_current->state (BLOCKED/SLEEPING) and linked it onto its
            // wait/timer queue; drop it from the run set and switch away. Safe for the
            // timer path (sleepq uses the separate tnext link); wait-queue callers must
            // detach_current() before parking (shared qnext/qprev links).
            detach_current();
            reschedule();
        }

        void wake(Thread* t)
        {
            IrqLock lock;
            if (t->state == ThreadState::READY || t->state == ThreadState::RUNNING)
            {
                return;
            }
            t->state = ThreadState::READY;
            rq_push_back(t);
            g_policy->on_ready(t);
            reschedule(); // switches now (thread ctx) or defers (ISR ctx) if warranted
        }

        void exit_current()
        {
            IrqLock lock;
            g_current->state = ThreadState::EXITED;
            rq_remove(g_current);
            g_policy->on_remove(g_current);
            if (g_current != g_idle && g_live > 0)
            {
                g_live--;
            }
            if (g_live == 0)
            {
                arch_shutdown(0);
            }
            reschedule(); // switch away permanently
            while (true)
            {
            } // unreachable: an EXITED thread is never picked again
        }

        Thread* current()
        {
            return g_current;
        }
        Thread* idle()
        {
            return g_idle;
        }
        unsigned live_count()
        {
            return g_live;
        }

        uint64_t next_slice_deadline()
        {
            if (g_current == nullptr)
            {
                return UINT64_MAX;
            }
            return g_current->slice_deadline_ns;
        }

        void tick_rr(uint64_t now)
        {
            IrqLock lock;
            Thread* c = g_current;
            if (c == nullptr)
            {
                return;
            }
            if (c->policy != Policy::RR || c->quantum_ns == 0)
            {
                return;
            }
            if (now < c->slice_deadline_ns)
            {
                return;
            }
            g_policy->on_slice_expire(c);
            // Grant a fresh quantum even when there is no equal-priority peer to
            // switch to; otherwise the expired deadline stays in the past and the
            // one-shot timer re-arms every minimum-delta (a ~50 kHz storm). If we
            // do switch, switch_to() re-arms for the incoming thread.
            arm_slice(c);
            reschedule();
        }

    }
}
