// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Scheduler core: run-state transitions, the single current thread, the one decision
// point (reschedule) and the context switch. The ready structure belongs to the active
// policy, never to this file; the default policy is in policy_fifo_rr.cc.

#include <kickos/sched.h>
#include <kickos/cap.h>
#include <kickos/console_tx.h> // console_on_driver_death
#include <kickos/kernel.h>
#include <kickos/domain.h>
#include <kickos/instance.h>
#include <kickos/time.h>
#include <kickos/irqlock.h>

namespace kickos
{
    namespace
    {
        // The one place a switch happens. Caller holds IrqLock.
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
            arch_mpu_apply(next->regions, next->region_count);
            // Must arm for the INCOMING thread before the jump: the outgoing thread does
            // not return here until it is itself resumed, so nothing else will program the
            // incoming thread's policy deadline (RR slice).
            ktime_rearm();
            arch_switch(&prev->ctx, &next->ctx);
        }
    }

    namespace sched
    {

        void init()
        {
            // No ready-structure reset here: it is policy-owned and zero-initialised with
            // the instance, Kernel being BSS.
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
            switch_to(next);
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
            // Blocking is legal only from thread context: from an ISR arch_switch defers
            // and the supposedly blocked thread keeps running.
            if (arch_in_isr())
            {
                kpanic(diag::kBlockInIsr);
            }
            kernel().policy->on_remove(kernel().current);
        }

        void block_current()
        {
            IrqLock lock;
            // Caller must already have set current->state and linked it onto its queue.
            // Safe for the timer path only, because sleepq uses the separate tnext link.
            // A wait-queue caller shares the ready/wait link node and must therefore
            // detach before linking, which is what wq_block does instead of coming here.
            detach_current();
            reschedule();
        }

        void wake(Thread* t)
        {
            IrqLock lock;
            // THE unpark funnel, and so the ONE place a timed wait's deadline is dropped.
            // Deliberately NOT in wq_pop_highest: a CALL_SEND_WAIT caller popped there goes
            // straight to reply_donor_park, a park-to-park migration that never becomes
            // ready, and a cancel there would strip the single deadline that must span both
            // call phases. Becoming ready IS this call, so an unpark that forgets to cancel
            // cannot be written.
            if (t->on_timer)
            {
                ktime_deadline_cancel(t);
            }
            if (t->state == ThreadState::READY or t->state == ThreadState::RUNNING)
            {
                return;
            }
            t->state = ThreadState::READY;
            kernel().policy->on_ready(t);
            Thread const* const c = kernel().current;
            // Null between sched::init and sched::start. No reachable pre-start waker exists
            // today; the test is here because tick_rr guards the same pointer and one
            // guarded reader plus one unguarded reader is the defect.
            if (c == nullptr)
            {
                return;
            }
            // EXITED means exit_current is past its own on_remove, so this thread will never
            // be picked again and a switch here would ABANDON the rest of exit_current: the
            // join waiters it has not reached yet would never be woken and kickos_terminate
            // would never run. Its own final reschedule is the switch.
            if (c->state == ThreadState::EXITED)
            {
                return;
            }
            // Inside the dying thread's cap_teardown sweep. This is a DECISION and not a fast
            // path: an RR slice expiry rotates the running thread behind its equals, and then
            // pick_next WOULD take an equal-priority peer. What makes admitting a strictly
            // higher-priority one safe is that the sweep drops IrqLock between chunks and c is
            // still on the ready structure, so it resumes and stays total. tick_rr already
            // switches an RR dying thread out at a chunk boundary with no dying test at all.
            // Reads kernel().current, which switch_to publishes BEFORE a deferred arch_switch:
            // after one admitted wake the dying thread keeps running with `c` naming the peer,
            // so later wakes in the same chunk are unguarded. Harmless today because the EPIPE
            // drain pops in descending priority, so pick_next returns the published peer and
            // reschedule early-returns.
            if (c->dying and t->prio <= c->prio)
            {
                return;
            }
            reschedule();
        }

        void set_prio(Thread* t, uint8_t p)
        {
            IrqLock lock;
            if (t->prio == p)
            {
                return;
            }
            // READY *and* RUNNING threads sit on a ready list keyed by t->prio, so both
            // must be re-seated: remove at the OLD prio, change it, re-add at the NEW one.
            // A bare t->prio write strands the node in the wrong per-prio list and desyncs
            // the bitmap; for a RUNNING thread it also leaves it at the head of its BOOSTED
            // list after a self-lower, so pick_next keeps returning it and the caller's
            // reschedule never switches. This does not reschedule: the caller must.
            if (t->state == ThreadState::READY or t->state == ThreadState::RUNNING)
            {
                kernel().policy->on_remove(t);
                t->prio = p;
                kernel().policy->on_ready(t);
                return;
            }
            // A BLOCKED thread is on no ready list, and neither queue it can be on is
            // prio-ordered: wq_pop_highest rescans at pop, the timer list is
            // deadline-sorted.
            t->prio = p;
        }

        void exit_current(int code)
        {
            Thread* const c = kernel().current;
            {
                IrqLock lock;
                // Deliberately not EXITED yet: EXITED is what makes the slot reclaimable,
                // and the sweep below runs with interrupts unmasked between chunks.
                // `state` cannot serve as the dying marker because a switch back in
                // rewrites it to RUNNING.
                c->dying = true;
                // BEFORE the sweep, not after. The sweep's endpoint arm EPIPE-wakes a
                // supervisor parked on this thread's endpoint, and that supervisor may
                // respawn immediately; the DEV-window exclusivity check (domain.h) refuses
                // while a live domain still holds the window. Only a refcount drop: the
                // thread keeps running off its own copied regions[], which no longer name
                // the Domain object.
                domain_release(c->domain);
            }
            // Must close every cap the exiting thread holds BEFORE its slot is reclaimable,
            // else object references leak (destroy-on-last-close). Preemptible: it drops
            // IrqLock between chunks so a large table cannot mask interrupts for the whole
            // sweep, and it is still TOTAL. See cap_teardown.
            cap_teardown(c);
            {
                IrqLock lock;
                Kernel& k = kernel();
                c->state = ThreadState::EXITED;
                // Must follow the whole teardown loop: every IRQ cap this thread held has
                // to be dropped, and every line it owned masked and detached, before the
                // device is re-initialised. A no-op unless this thread was the last
                // receiver on the PUBLISHED console endpoint (see console_tx.h). Deferred
                // again while a CONCURRENT sweep is in flight; the note is sticky, so that
                // thread's own exit runs it.
                if (not cap_teardown_active())
                {
                    console_on_driver_death();
                }
                k.policy->on_remove(c);
                if (c != k.idle and k.live > 0)
                {
                    k.live--;
                }
                // Join and wait-until-last are parked on NO list, so this pool scan IS the
                // waiter lookup; it runs at a thread exit and nowhere else. The wait edge
                // and wait_result are the waker's to write BEFORE the wake, as on every
                // wait queue. `state` is EXITED by now, which is the clause in wake that
                // suppresses these switches; `dying` no longer does it, because a joiner may
                // outrank this thread. The reschedule further down stays the single switch
                // away, and a switch from inside this loop would abandon the rest of it.
                bool const last_out = (k.live == 1);
                for (int s = 0; s < k.threads.next; s++)
                {
                    Thread* const w = &k.threads.slots[s];
                    if (w->wait_join_target() == c)
                    {
                        w->clear_wait_edge();
                        w->wait_result = 0;
                        wake(w);
                        continue;
                    }
                    // A parked waiter is still counted live, so a count of 1 here names
                    // that waiter and nothing else.
                    if (last_out and w->wait_kind == WAIT_LIVE_LAST)
                    {
                        // No wait_result: reaching this wake IS the whole answer, and
                        // thread_wait_last returns 0 without reading one.
                        w->clear_wait_edge();
                        wake(w);
                    }
                }
                if (k.live == 0)
                {
                    // Last non-idle thread out ends the process with its exit code.
                    kickos_terminate(code);
                }
                reschedule();
            }
            // Not unreachable. On an arch that defers the switch (ARM PendSV) the switch
            // fires as `lock` is destroyed, not inside it, so control does reach here with
            // the switch merely pending. An EXITED thread is off the ready set and is never
            // scheduled again, so parking is the only correct thing to do.
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
            // The deadline is the policy's; a policy with no timed event (a FIFO thread)
            // reports UINT64_MAX and lands in this early return.
            if (now < kernel().policy->next_timed_event())
            {
                return;
            }
            kernel().policy->on_slice_expire(c);
            reschedule();
        }

    }
}
