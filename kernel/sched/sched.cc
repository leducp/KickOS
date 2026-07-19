// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Scheduler core (pure mechanism): run-state transitions, the single current
// thread, one decision point (reschedule), and the context switch. The active
// policy decides which thread runs and owns the ready structure (see sched.h);
// the default FIFO/RR policy lives in policy_fifo_rr.cc.

#include <kickos/sched.h>
#include <kickos/cap.h>
#include <kickos/kernel.h>
#include <kickos/domain.h>
#include <kickos/instance.h>
#include <kickos/time.h>
#include <kickos/irqlock.h>
#include <kickos/console_tx.h>

namespace kickos
{
    namespace
    {
        // The one place a switch happens. Caller holds the critical section.
        void switch_to(Thread* next)
        {
            Thread* prev = kernel().current;
            if (prev->state == ThreadState::RUNNING)
            {
                prev->state = ThreadState::READY;
            }
            kernel().current = next;
            next->state = ThreadState::RUNNING;
            next->switch_count++;
            kernel().policy->on_switch_in(next);
            // Task-switch hook: reprogram per-task MPU regions in one place.
            arch_mpu_apply(next->regions, next->region_count);
            // Arm the next-event timer for the incoming thread BEFORE we jump: the
            // switching-out thread will not return here until it is resumed, so its
            // policy deadlines (RR slice) must be programmed now.
            ktime_rearm();
            arch_switch(&prev->ctx, &next->ctx);
        }
    }

    namespace sched
    {

        void init()
        {
            // The ready structure is policy-owned and zero-initialized with the
            // instance (Kernel is BSS); the core resets only its own run state.
            Kernel& k = kernel();
            k.current = nullptr;
            k.idle = nullptr;
            k.live = 0;
            k.policy = default_policy();
        }

        void set_policy(SchedPolicy const* policy)
        {
            kernel().policy = policy;
        }

        void add(Thread* t)
        {
            IrqLock lock;
            t->state = ThreadState::READY;
            kernel().policy->on_ready(t);
            if (t->prio == KICKOS_PRIO_IDLE)
            {
                kernel().idle = t;
            }
            else
            {
                kernel().live++;
            }
        }

        void start()
        {
            IrqLock lock;
            Thread* first = kernel().policy->pick_next();
            kernel().current = first;
            first->state = ThreadState::RUNNING;
            kernel().policy->on_switch_in(first);
            arch_mpu_apply(first->regions, first->region_count);
            ktime_rearm();
            arch_start(&kernel().boot, &first->ctx);
        }

        void reschedule()
        {
            IrqLock lock;
            Thread* next = kernel().policy->pick_next();
            if (next == kernel().current)
            {
                return;
            }
            switch_to(next); // arms the next-event timer for the incoming thread
        }

        void yield()
        {
            IrqLock lock;
            kernel().policy->on_yield(kernel().current);
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
            kernel().policy->on_remove(kernel().current);
        }

        void block_current()
        {
            IrqLock lock;
            // Caller has set current->state (BLOCKED/SLEEPING) and linked it onto its
            // wait/timer queue; drop it from the run set and switch away. The timer
            // path is safe (sleepq uses the separate tnext link); wait-queue callers
            // must detach_current() first (shared ready/wait link node).
            detach_current();
            reschedule();
        }

        void wake(Thread* t)
        {
            IrqLock lock;
            if (t->state == ThreadState::READY or t->state == ThreadState::RUNNING)
            {
                return;
            }
            t->state = ThreadState::READY;
            kernel().policy->on_ready(t); // enqueue on the ready structure
            // If the CURRENT thread is EXITED we are inside cap_teardown on the exit
            // path (the mutex force-unlock, and #4's endpoint EPIPE-wake, wake from
            // there). Do NOT switch away now: the EXITED thread is still on the ready
            // structure (on_remove runs later in exit_current), so a switch would let
            // ThreadPool::alloc reclaim its slot -> ready-list corruption / UAF, or
            // strand it. exit_current does the real switch via its own final
            // reschedule after on_remove. (Previously safe only by a delicate PI
            // invariant -- dying owner's prio >= its transfer target -- which #4's
            // EPIPE-wake would not satisfy.)
            if (kernel().current->state == ThreadState::EXITED)
            {
                return;
            }
            reschedule(); // switches now (thread ctx) or defers (ISR ctx) if warranted
        }

        void set_prio(Thread* t, uint8_t p)
        {
            IrqLock lock;
            if (t->prio == p)
            {
                return;
            }
            // READY *and* RUNNING threads are on a ready list keyed by t->prio (the
            // running thread sits at the front of ready[prio]). Both must be re-seated:
            // remove at the OLD prio, change it, re-add at the NEW one (via the policy
            // hooks). A bare t->prio write would strand the node in the wrong per-prio
            // list and desync the bitmap -- and for a RUNNING thread would leave it at
            // the head of its BOOSTED list after a self-lower, so pick_next keeps
            // returning it and the subsequent reschedule never switches. Re-add is
            // push_back: the thread lands at the tail of its new level. The caller
            // reschedules right after (a lowered RUNNING thread may now yield the CPU).
            if (t->state == ThreadState::READY or t->state == ThreadState::RUNNING)
            {
                kernel().policy->on_remove(t);
                t->prio = p;
                kernel().policy->on_ready(t);
                return;
            }
            // BLOCKED (wq_pop_highest rescans at pop) and SLEEPING (timer list is
            // deadline-sorted, prio-independent) are on no ready list: just take it.
            t->prio = p;
        }

        void exit_current(int code)
        {
            {
                IrqLock lock;
                Kernel& k = kernel();
                k.current->state = ThreadState::EXITED;
                // Close every cap the exiting thread holds BEFORE its slot is
                // reclaimable, else it leaks object references (destroy-on-last-close).
                cap_teardown(k.current);
                domain_release(k.current->domain); // last thread out frees the domain
                k.policy->on_remove(k.current);
                if (k.current != k.idle and k.live > 0)
                {
                    k.live--;
                }
                if (k.live == 0)
                {
                    // Last non-idle thread out: end the process with its exit code.
                    // Flush the buffered console first so trailing output is not
                    // stranded in the ring (kpanic/fault already flush).
                    console_tx_flush_sync();
                    arch_shutdown(code);
                }
                reschedule(); // pick the successor + pend the switch away
            }
            // The switch away is now committed. On the sim arch_switch already
            // swapped synchronously and never returned here. On an arch that defers
            // the switch (ARM PendSV), it can only fire once the crit section above
            // releases -- so it fires as `lock` is destroyed, NOT inside it. An
            // EXITED thread is off the ready set, so it is never scheduled again;
            // park until the pended switch takes effect. (Running past here with the
            // switch merely pending is the bug this replaced: the old
            // KICKOS_UNREACHABLE executed, unmasked, before the deferred PendSV landed.)
            while (true)
            {
                arch_idle_wait();
            }
        }

        Thread* current()
        {
            return kernel().current;
        }
        Thread* idle()
        {
            return kernel().idle;
        }
        unsigned live_count()
        {
            return kernel().live;
        }

        uint64_t next_timed_event()
        {
            return kernel().policy->next_timed_event();
        }

        void tick_rr(uint64_t now)
        {
            IrqLock lock;
            Thread* c = kernel().current;
            if (c == nullptr)
            {
                return;
            }
            // The policy owns the deadline; the core owns the clock. UINT64_MAX
            // (no timed event, e.g. a FIFO thread) makes this early-return.
            if (now < kernel().policy->next_timed_event())
            {
                return;
            }
            // Policy acts (RR: rotate + re-grant the quantum), then we reschedule.
            kernel().policy->on_slice_expire(c);
            reschedule();
        }

    }
}
