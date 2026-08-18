// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Scheduler core: run-state transitions, the single current thread, the one decision
// point (reschedule) and the context switch. The ready structure belongs to the active
// policy, never to this file; the default policy is in policy_fifo_rr.cc.

#include <kickos/sched.h>
#include <kickos/bench.h>
#include <kickos/cap.h>
#include <kickos/console_tx.h> // console_on_driver_death
#include <kickos/kernel.h>
#include <kickos/instance.h>
#include <kickos/task.h>
#include <kickos/time.h>
#include <kickos/irqlock.h>

#include <kickos/sys/abi.h> // KOS_EXIT_CANCELLED: the slay stub's exit code

namespace kickos
{
    namespace
    {
        // The one place a switch happens. Caller holds IrqLock.
        //
        // `prev` is the PUBLISHED thread, which a switch pended earlier under this same lock
        // has already moved off the executing one. `from` is right either way: the backends
        // that pend ignore it and save whatever the switcher finds, and the two that swap
        // inline cannot have a pend outstanding. A publication superseded that way keeps the
        // switch_count and the RR slice armed here; whether a pend has fired is knowable only
        // to the arch.
        void switch_to(Thread* next)
        {
            KICKOS_BENCH_MARK(bm_book);
            Thread* prev = kernel().current;
            if (prev->state == ThreadState::RUNNING)
            {
                prev->state = ThreadState::READY;
            }
            kernel().current = next;
            next->state = ThreadState::RUNNING;
            next->switch_count.store(next->switch_count.load() + 1u);
            kernel().policy->on_switch_in(next);
            KICKOS_BENCH_SPAN(PH_SWITCH_BOOK, bm_book);
            KICKOS_BENCH_MARK(bm_mpu);
            arch_mpu_apply(next->regions, next->region_count);
            KICKOS_BENCH_SPAN(PH_MPU_APPLY, bm_mpu);
            // Must arm for the INCOMING thread before the jump: the outgoing thread does
            // not return here until it is itself resumed, so nothing else will program the
            // incoming thread's policy deadline (RR slice).
            ktime_rearm();
            // CLAIM THE RESUME of a slain thread. Only the INCOMING context, and only
            // before arch_switch: the deferred switchers (PendSV, .Lswitch, _kickos_rx_pendsw,
            // _kickos_int_level1) SAVE the outgoing thread's live registers over prev->ctx and
            // RESTORE next->ctx, so a rebuild of prev->ctx here would be overwritten and
            // silently lost. `dying` is the restart guard and not a second flag: cap_teardown
            // releases IrqLock between chunks, so a half-swept thread is preemptible and
            // re-entering the stub from the top would restart a sweep over a table that is
            // already partly empty. Idempotent in its values, so a rebuild repeated before the
            // stub runs changes nothing.
            if (next->cancel_kind == CANCEL_SLAY and not next->dying)
            {
                arch_ctx_redirect(&next->ctx, kickos_thread_slay_exit, next->stack_base,
                                  next->stack_size);
            }
            KICKOS_BENCH_MARK(bm_arch);
            arch_switch(&prev->ctx, &next->ctx);
            KICKOS_BENCH_SPAN(PH_ARCH_SWITCH, bm_arch);
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
            KICKOS_BENCH_MARK(bm_pick);
            Thread* next = kernel().policy->pick_next();
            KICKOS_BENCH_SPAN(PH_PICK_NEXT, bm_pick);
            if (next == kernel().current)
            {
                return;
            }
            // Backends that PEND (armv7m, rv32imac, rxv3) return from arch_switch at once,
            // so this brackets the bookkeeping only. The LX6 and the sim swap inline from
            // thread context, where it instead ends when this thread is next resumed.
            KICKOS_BENCH_MARK(bm_switch);
            switch_to(next);
            KICKOS_BENCH_SPAN(PH_SWITCH_TO, bm_switch);
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
            // Closes only on the readying path: the two refusals below do no ready-queue
            // work, so counting them would pull this phase's min toward zero.
            KICKOS_BENCH_MARK(bm_unpark);
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
            // BLOCKED is the only state a park leaves behind, and every other one has to be
            // refused rather than readied: EXITED is what ThreadPool::alloc reads as a free
            // slot, so readying an exited thread takes that slot out of the pool for good.
            if (t->state != ThreadState::BLOCKED)
            {
                return;
            }
            t->state = ThreadState::READY;
            kernel().policy->on_ready(t);
            KICKOS_BENCH_SPAN(PH_WAKE_UNPARK, bm_unpark);
            Thread const* const c = kernel().current;
            // Null between sched::init and sched::start.
            if (c == nullptr)
            {
                return;
            }
            // `c` is what runs when the mask lifts, which is this thread only until one of the
            // switches below is admitted: on a backend that pends, the sweep keeps running with
            // `current` naming its peer, so a later wake under the same IrqLock reads that peer
            // and both clauses decline. What holds the decision is pick_next, which still
            // returns that peer: it outranks the sweep and heads the highest ready list.
            //
            // Never picked again, so a switch here abandons the rest of exit_current: its
            // remaining waiters go unwoken. Its own final reschedule is the switch. That
            // abandonment needs a backend that swaps inline, where `c` is never a peer.
            if (c->state == ThreadState::EXITED)
            {
                return;
            }
            // Not an optimisation: an RR slice expiry can rotate the dying thread off its
            // ready-list head, and pick_next would then take an equal-priority peer.
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
            // The group this thread leaves, and whether leaving it EMPTIED the group.
            // Function-scoped because the WAIT_TASK_EMPTY sweep that reads them runs in the
            // second locked block, on the far side of a preemptible cap_teardown.
            //
            // `left_task` survives that gap as a bare POINTER, and what makes the compare
            // there unambiguous lives in another file: an IMPLICIT task's slot is freed by
            // the release below and could be re-handed mid-sweep, but task_resolve refuses a
            // creator-less slot (task.cc), so no caller can ever hold a handle to park
            // WAIT_TASK_EMPTY against one. An EXPLICIT task's slot is held by its creator
            // until the wait resolves. Do not weaken either half without revisiting this.
            Task* left_task = nullptr;
            bool emptied_task = false;
            {
                IrqLock lock;
                // Deliberately not EXITED yet: EXITED is what makes the slot reclaimable,
                // and the sweep below runs with interrupts unmasked between chunks.
                // `state` cannot serve as the dying marker because a switch back in
                // rewrites it to RUNNING.
                c->dying = true;
                // TASK-SCOPED DEATH. A thread that joined a task declared itself part of one
                // unit, so its peers go with it; under the implicit one-thread-per-task
                // default the scan matches nobody and this is inert, which is what makes the
                // rule safe for every spawn written before tasks existed
                // (docs/design-task-layer.md section 6). It must precede task_release, which
                // may free the slot and leave c->task a dangling name.
                //
                // THE KIND TRAVELS WITH THE DEATH. A member that was slain slays its peers,
                // or the group dies by two rules and a peer keeps a cleanup window the
                // supervisor already denied its sibling. Any other exit, a return from
                // main, a fault or a cooperative kill, ends the group cooperatively, which is
                // what the floor at CANCEL_KILL says.
                uint8_t group_kind = CANCEL_KILL;
                if (c->cancel_kind > group_kind)
                {
                    group_kind = c->cancel_kind;
                }
                task_cancel_group(c->task, group_kind);
                // BEFORE the sweep, not after. The sweep's endpoint arm EPIPE-wakes a
                // supervisor parked on this thread's endpoint, and that supervisor may
                // respawn immediately; the DEV-window exclusivity check (kernel.h) refuses
                // while a live thread still holds the window, and `dying` above is what
                // takes this one out of that scan. Only a refcount drop: the thread keeps
                // running off its own copied regions[], which no longer name the Task or the
                // Domain object.
                // Captured BEFORE the name is retired below, because the WAIT_TASK_EMPTY
                // sweep further down runs after cap_teardown and needs the group this
                // thread LEFT. Reading the refcount there instead would be wrong twice: an
                // implicit task's slot is freed inside this call, and a slot at zero cannot
                // say whether THIS death is what took it there.
                left_task = c->task;
                emptied_task = task_release(c->task);
                // RETIRE THE NAME WITH THE REFERENCE. The slot may be free now, and
                // free_slot() can re-hand it in the sweep's first chunk gap. Membership in
                // task_cancel_group is a POINTER COMPARISON, so a stale c->task would make
                // this thread a phantom member of whatever group lands here next, caught
                // today only by thread_cancel's `dying` test, whose documented job is
                // stopping two co-dying members from marking each other, not covering slot
                // reuse. task_cancel_group(nullptr) and task_domain(nullptr) are both total.
                c->task = nullptr;
                // The creator's hold ends with the creator. Keyed on the TAG, because the tag
                // IS the gate: a recycled pool slot answers kill_tag_of with its predecessor's,
                // so a hold left behind is creator authority its successor never earned. The
                // group is not cancelled: a spawner's death does not kill its children, and a
                // member's own exit already ends the group.
                task_orphan_created_by(kernel().threads.kill_tag_of(c));
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
                // The RETRY, and the only site that needs one: the reclaim already ran at the
                // note (cap.cc), in the window that EPIPEd the peer. A refusal there means a
                // live thread still held the register window, and only a death frees it.
                // Deferred while a CONCURRENT sweep is in flight; the note is sticky.
                if (not cap_teardown_active())
                {
                    console_on_driver_death();
                }
                k.policy->on_remove(c);
                if (c != k.idle and k.live > 0)
                {
                    k.live--;
                }
                // Join, wait-until-last and wait-for-the-group-to-empty are parked on NO
                // list, so this pool scan IS the waiter lookup; it runs at a thread exit and
                // nowhere else. The wait edge
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
                    // Only when THIS death emptied the group, so a member leaving a group
                    // that still has peers wakes nobody. The waiter's own group is compared
                    // by pointer, exactly as membership is.
                    if (emptied_task and w->wait_task_target() == left_task)
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

// Reached only through a context arch_ctx_redirect rebuilt, so `current` IS the slain
// thread and this runs privileged at the top of its own stack. The window before
// exit_current's first IrqLock is a handful of instructions with interrupts on; a
// preemption there costs one idempotent rebuild of work that has not happened.
//
// Prints nothing, unlike kickos_thread_fault_exit: a fault is an event a supervisor cannot
// otherwise learn about, and a slay was asked for by a thread that already knows. A banner
// would also route this path through kprintf_fault's published-console delivery, whose
// ordering guarantee rests on a console driver provisioned at or above every stdout client.
extern "C" void kickos_thread_slay_exit(void* arg)
{
    (void)arg;
    ::kickos::sched::exit_current(KOS_EXIT_CANCELLED);
}
