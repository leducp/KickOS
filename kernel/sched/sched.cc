// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Scheduler core: run-state transitions, the single current thread, the one decision
// point (reschedule) and the context switch. The ready structure is policy-owned, never
// this file's.

#include <kickos/sched.h>
#include <kickos/bench.h>
#include <kickos/cap.h>
#include <kickos/console_tx.h> // console_on_driver_death
#include <kickos/kernel.h>
#include <kickos/instance.h>
#include <kickos/task.h>
#include <kickos/time.h>
#include <kickos/irqlock.h>

#include <kickos/sys/abi.h> // KOS_EXIT_CANCELLED

namespace kickos
{
    namespace
    {
        // The one place a switch happens. Caller holds IrqLock.
        // `prev` is the PUBLISHED thread, not necessarily the executing one: a switch pended
        // earlier under this same lock has already moved off it.
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
            // Must arm for the INCOMING thread before the jump: nothing else will program
            // its policy deadline (RR slice).
            ktime_rearm();
            // CLAIM THE RESUME of a slain thread: INCOMING context only, and only before
            // arch_switch. The deferred switchers save the outgoing thread's live registers
            // over prev->ctx, so a rebuild of prev->ctx here would be overwritten.
            // `dying` is the restart guard: cap_teardown releases IrqLock between chunks, so
            // re-entering the stub from the top would restart a partly-done sweep.
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
            // No ready-structure reset here: policy-owned, and zeroed with the BSS instance.
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
            // Blocking is legal only from thread context: from an ISR the switch defers and
            // the supposedly blocked thread keeps running.
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
            // Timer path only: sleepq uses the separate tnext link. A wait-queue caller
            // shares the ready/wait link node and must detach before linking (wq_block).
            detach_current();
            reschedule();
        }

        bool wake_no_resched(Thread* t)
        {
            IrqLock lock;
            // Spans the readying path only: the refusals below do no ready-queue work.
            KICKOS_BENCH_MARK(bm_unpark);
            // THE unpark funnel, and so the ONE place a timed wait's deadline is dropped.
            // NOT in wq_pop_highest: a CALL_SEND_WAIT caller popped there migrates park to
            // park, and a cancel there would strip the deadline spanning both call phases.
            if (t->on_timer)
            {
                ktime_deadline_cancel(t);
            }
            // Every other state must be refused: EXITED is what ThreadPool::alloc reads as a
            // free slot, so readying an exited thread takes that slot out of the pool for good.
            if (t->state != ThreadState::BLOCKED)
            {
                return false;
            }
            t->state = ThreadState::READY;
            kernel().policy->on_ready(t);
            KICKOS_BENCH_SPAN(PH_WAKE_UNPARK, bm_unpark);
            return true;
        }

        void resched_after_wake(Thread const* t)
        {
            IrqLock lock;
            Thread const* const c = kernel().current;
            // Null between sched::init and sched::start.
            if (c == nullptr)
            {
                return;
            }
            // A switch here would abandon the rest of exit_current and leave its remaining
            // waiters unwoken; that thread's own final reschedule is the switch.
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

        void wake(Thread* t)
        {
            IrqLock lock;
            if (wake_no_resched(t))
            {
                resched_after_wake(t);
            }
        }

        void set_prio(Thread* t, uint8_t p)
        {
            IrqLock lock;
            if (t->prio == p)
            {
                return;
            }
            // READY *and* RUNNING threads sit on a ready list keyed by t->prio: remove at
            // the OLD prio, change it, re-add at the NEW one. A bare t->prio write strands
            // the node in the wrong per-prio list and desyncs the bitmap. Does not
            // reschedule; the caller must.
            if (t->state == ThreadState::READY or t->state == ThreadState::RUNNING)
            {
                kernel().policy->on_remove(t);
                t->prio = p;
                kernel().policy->on_ready(t);
                return;
            }
            // A BLOCKED thread is on no ready list, and neither queue it can be on is
            // prio-ordered: wq_pop_highest rescans at pop, the timer list is deadline-sorted.
            t->prio = p;
        }

        void exit_current(int code)
        {
            Thread* const c = kernel().current;
            // Read by the WAIT_TASK_EMPTY sweep in the second locked block, past a
            // preemptible cap_teardown. `left_task` crosses that gap as a bare POINTER, and
            // what keeps the compare unambiguous lives in task.cc: task_resolve refuses a
            // creator-less slot, so an implicit task's freed slot can never be the target of
            // a parked WAIT_TASK_EMPTY. Do not weaken that without revisiting this.
            Task* left_task = nullptr;
            bool emptied_task = false;
            {
                IrqLock lock;
                // Not EXITED yet: EXITED makes the slot reclaimable, and the sweep below runs
                // with interrupts unmasked between chunks. `state` cannot be the dying marker,
                // a switch back in rewrites it to RUNNING.
                c->dying = true;
                // TASK-SCOPED DEATH (docs/design-task-layer.md section 6). Must precede
                // task_release, which may free the slot and leave c->task a dangling name.
                // THE KIND TRAVELS WITH THE DEATH: a slain member slays its peers, and the
                // floor at CANCEL_KILL ends the group cooperatively for every other exit.
                uint8_t group_kind = CANCEL_KILL;
                if (c->cancel_kind > group_kind)
                {
                    group_kind = c->cancel_kind;
                }
                task_cancel_group(c->task, group_kind);
                // BEFORE the sweep, not after: the sweep's endpoint arm EPIPE-wakes a
                // supervisor that may respawn immediately, and the DEV-window exclusivity
                // check (kernel.h) refuses while a live thread holds the window; `dying`
                // above is what takes this one out of that scan.
                // `left_task` is captured before the name is retired below; a refcount read
                // in the sweep cannot say whether THIS death is what emptied the group.
                left_task = c->task;
                emptied_task = task_release(c->task);
                // RETIRE THE NAME WITH THE REFERENCE. The slot may be free now and can be
                // re-handed in the sweep's first chunk gap; membership is a POINTER
                // COMPARISON, so a stale c->task makes this thread a phantom member of
                // whatever group lands there next. Both take nullptr.
                c->task = nullptr;
                // The creator's hold ends with the creator, keyed on the TAG: a recycled pool
                // slot answers kill_tag_of with its predecessor's, so a hold left behind is
                // creator authority its successor never earned. The group is NOT cancelled.
                task_orphan_created_by(kernel().threads.kill_tag_of(c));
            }
            // Must close every cap the exiting thread holds BEFORE its slot is reclaimable,
            // else object references leak (destroy-on-last-close). Preemptible: it drops
            // IrqLock between chunks.
            cap_teardown(c);
            {
                IrqLock lock;
                Kernel& k = kernel();
                c->state = ThreadState::EXITED;
                // The RETRY, and the only site that needs one: the reclaim already ran at the
                // note (cap.cc), and a refusal there means a live thread still held the
                // register window. Deferred while a CONCURRENT sweep is in flight; the note
                // is sticky.
                if (not cap_teardown_active())
                {
                    console_on_driver_death();
                }
                k.policy->on_remove(c);
                if (c != k.idle and k.live > 0)
                {
                    k.live--;
                }
                // Join, wait-until-last and wait-for-the-group-to-empty park on NO list, so
                // this pool scan IS the waiter lookup. The wait edge and wait_result are the
                // waker's to write BEFORE the wake, as on every wait queue. `state` is EXITED
                // by now, which is the clause in resched_after_wake that suppresses a switch
                // from inside this loop; such a switch would abandon the rest of it.
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
                    // Only when THIS death emptied the group. Compared by pointer, exactly
                    // as membership is.
                    if (emptied_task and w->wait_task_target() == left_task)
                    {
                        w->clear_wait_edge();
                        w->wait_result = 0;
                        wake(w);
                        continue;
                    }
                    // A parked waiter is still counted live, so a count of 1 names it.
                    if (last_out and w->wait_kind == WAIT_LIVE_LAST)
                    {
                        // No wait_result: thread_wait_last returns 0 without reading one.
                        w->clear_wait_edge();
                        wake(w);
                    }
                }
                if (k.live == 0)
                {
                    kickos_terminate(code);
                }
                reschedule();
            }
            // Not unreachable: where the switch is deferred it fires as `lock` is destroyed,
            // so control reaches here with the switch merely pending.
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
            // A policy with no timed event reports UINT64_MAX.
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
// thread and this runs privileged at the top of its own stack.
extern "C" void kickos_thread_slay_exit(void* arg)
{
    (void)arg;
    ::kickos::sched::exit_current(KOS_EXIT_CANCELLED);
}
