// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Scheduler core (pure mechanism): run-state transitions, the single current
// thread, one decision point (reschedule), and the context switch. The active
// policy decides which thread runs and owns the ready structure (see sched.h);
// the default FIFO/RR policy lives in policy_fifo_rr.cc.

#include <kickos/sched.h>
#include <kickos/kernel.h>
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
            reschedule();                 // switches now (thread ctx) or defers (ISR ctx) if warranted
        }

        void exit_current(int code)
        {
            {
                IrqLock lock;
                Kernel& k = kernel();
                k.current->state = ThreadState::EXITED;
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
            for (;;)
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
