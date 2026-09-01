// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The ready structure is policy-owned, never this file's.

#include <kickos/aspace.h>
#include <kickos/sched.h>
#include <kickos/bench.h>
#include <kickos/cap.h>
#include <kickos/console_tx.h>
#include <kickos/kernel.h>
#include <kickos/instance.h>
#include <kickos/domain.h>
#include <kickos/task.h>
#include <kickos/time.h>
#include <kickos/irqlock.h>
#include <kickos/klock.h>
#include <kickos/reent.h>
#include <kickos/ustack.h>

#include <kickos/sys/abi.h>

namespace kickos
{
    namespace
    {
#if KICKOS_LIBC_REENT
        // The two lookups are compiled out where the backend translates nothing: leaving
        // them on the switch path spends two out-of-line calls per switch to produce a
        // constant.
        struct arch_aspace* reent_space_of(Thread const* t)
        {
#if KICKOS_HAVE_ASPACE
            return domain_space(task_domain(t->task));
#else
            (void)t;
            return nullptr;
#endif
        }
#endif

        // Caller holds IrqLock. `prev` is the PUBLISHED thread, not necessarily the
        // executing one: a switch pended earlier under this same lock has already moved off
        // it. always_inline keeps the split from moving a call frame into or out of the
        // brackets below.
        inline __attribute__((always_inline)) void switch_book(Thread* next)
        {
            KICKOS_BENCH_MARK(bm_book);
            Thread* prev = kernel().current[kickos_kernel_core()];
            if (prev->state == ThreadState::RUNNING)
            {
                prev->state = ThreadState::READY;
            }
            kernel().current[kickos_kernel_core()] = next;
            next->state = ThreadState::RUNNING;
            next->switch_count.store(next->switch_count.load() + 1u);
            kernel().policy->on_switch_in(next);
            KICKOS_BENCH_SPAN(PH_SWITCH_BOOK, bm_book);
            KICKOS_BENCH_MARK(bm_mpu);
            // What the incoming thread may touch is installed here and nowhere else.
            aspace_activate_for(next);
            next->mpu.apply();
            KICKOS_BENCH_SPAN(PH_MPU_APPLY, bm_mpu);
#if KICKOS_LIBC_REENT
            // After mpu.apply, never before: this and the prime it may do both write memory
            // the incoming thread owns, so they belong on the far side of its memory view
            // being installed. And only where that view IS the incoming thread's: both
            // addresses live in the half a translating backend switches per process, so for a
            // thread whose task holds no space they name whichever process was installed last
            // (kickos/aspace.h, aspace_seated_for).
            if (aspace_seated_for(next))
            {
                struct arch_aspace* const rspace = reent_space_of(next);
                if (next->reent_fresh)
                {
                    next->reent_fresh = false;
                    reent_prime(rspace, next->reent);
                }
                reent_seat(rspace, next->reent);
            }
#endif
            // Must arm for the INCOMING thread before the jump: nothing else will program
            // its policy deadline (RR slice).
            ktime_rearm();
            // Claims the resume of a slain thread: incoming context only, and only before
            // arch_switch, the deferred switchers saving the outgoing thread's live registers
            // over prev->ctx. `dying` is the restart guard: cap_teardown releases IrqLock
            // between chunks, so re-entering the stub from the top would restart a
            // partly-done sweep.
            if (next->cancel_kind == CANCEL_SLAY and not next->dying)
            {
                arch_ctx_redirect(&next->ctx, kickos_thread_slay_exit, next->stack_base,
                                  next->stack_size);
            }
#if KICKOS_ARCH_HAS_IPC_FASTPATH
            // A fastpath-parked thread has no kernel continuation to hand wait_result back
            // through, so this switch is the last place the result can reach its saved frame.
            if (next->call_frame_parked != 0)
            {
                next->call_frame_parked = 0;
                arch_ctx_set_syscall_result(&next->ctx,
                                            static_cast<uint32_t>(next->wait_result));
            }
#endif
        }

        // Returns the context to resume, or nullptr when containment is refused.
        extern "C" struct arch_context* kickos_thread_contain_wild_stack(
            struct arch_context* offender, char const** slain_name)
        {
            if (offender == nullptr)
            {
                return nullptr;
            }
            // Taken here and not by the caller: a trap prologue reaches this unmasked,
            // PendSV being entered with PRIMASK and BASEPRI both clear.
            IrqLock lock;
            Kernel& k = kernel();
            uint32_t const cpu = arch_cpu_id();
            if (k.current[cpu] == nullptr)
            {
                return nullptr;
            }
            // A context no slot owns is the boot context or a freed one, and neither is a
            // thread to contain.
            Thread* bad = nullptr;
            for (int s = 0; s < k.threads.next; s++)
            {
                if (&k.threads.slots[s].ctx == offender)
                {
                    bad = &k.threads.slots[s];
                    break;
                }
            }
            if (bad == nullptr)
            {
                return nullptr;
            }
            // A privileged thread's wild pointer is a kernel bug, and containment would hide
            // it behind a thread death. Idle needs no clause of its own and MUST NOT grow
            // one: it is kernel().idle_tcb and not a pool slot, so the scan above already
            // returned null for it (tests/unit/wildstack).
            if (bad->privileged)
            {
                return nullptr;
            }
            thread_cancel_kind(bad, CANCEL_SLAY);
            // Containment rests entirely on the claim landing. The save half never ran, so
            // ctx.sp still describes a frame from some earlier switch; what makes that
            // harmless is switch_book rebuilding the context on the way back in, which it
            // does only for a thread the slay actually claimed.
            if (bad->cancel_kind != CANCEL_SLAY or bad->dying)
            {
                return nullptr;
            }
            // Read here while the slot is still the slain thread's: its teardown runs only
            // once the caller resumes it.
            if (slain_name != nullptr)
            {
                *slain_name = bad->name;
            }
            // A switch booked before the trap has already moved `current` off the offender
            // and stashed the incoming thread's regions.
            if (k.current[cpu] == bad)
            {
                Thread* const next = k.policy->pick_next();
                if (next == nullptr)
                {
                    return nullptr;
                }
                switch_book(next);
            }
            return &k.current[cpu]->ctx;
        }

        void switch_to(Thread* next)
        {
            Thread* prev = kernel().current[kickos_kernel_core()];
            switch_book(next);
            // THE KERNEL LOCK SPANS THE SWAP: until the swap parks it, the outgoing thread's
            // saved frame base still describes an earlier run, and kickos_switch_unlock is
            // what ends the span. The depth rides on this frame, travelling with the thread.
            uint32_t const klock_depth = klock_detach();
            KICKOS_BENCH_MARK(bm_arch);
            arch_switch(&prev->ctx, &next->ctx);
            KICKOS_BENCH_SPAN(PH_ARCH_SWITCH, bm_arch);
            klock_attach(klock_depth);
        }
    }

    namespace sched
    {

        void init()
        {
            // No ready-structure reset here: policy-owned, and zeroed with the BSS instance.
            Kernel& k = kernel();
            k.current[kickos_kernel_core()] = nullptr;
            k.idle[kickos_kernel_core()] = nullptr;
            k.live = 0;
            k.policy = default_policy();
        }

        void set_policy(SchedPolicy const* policy)
        {
            kernel().policy = policy;
        }

#if KICKOS_KERNEL_CORES > 1
        namespace
        {
            // Raise a reschedule on every peer running below `prio`, so it looks at the
            // ready lists.
            void poke_peers_below(uint8_t prio)
            {
                uint32_t const me = kickos_kernel_core();
                uint32_t peers = 0;
                for (uint32_t core = 0; core < KICKOS_KERNEL_CORES; core++)
                {
                    Thread const* const running = kernel().current[core];
                    if (core == me or running == nullptr)
                    {
                        continue;
                    }
                    if (running->prio < prio)
                    {
                        peers |= 1u << core;
                    }
                }
                if (peers != 0)
                {
                    klock_resched_ask(peers);
                }
            }
        }
#endif

        void add(Thread* t)
        {
            IrqLock lock;
            t->state = ThreadState::READY;
            kernel().policy->on_ready(t);
            if (t->prio == KICKOS_PRIO_IDLE)
            {
                kernel().idle[kickos_kernel_core()] = t;
            }
            else
            {
                kernel().live++;
            }
#if KICKOS_KERNEL_CORES > 1
            poke_peers_below(t->prio);
#endif
        }

#if KICKOS_KERNEL_CORES > 1
        void add_idle(Thread* t, uint32_t core)
        {
            IrqLock lock;
            t->state = ThreadState::READY;
            kernel().policy->on_ready(t);
            kernel().idle[core] = t;
        }
#endif

        void start()
        {
            IrqLock lock;
            Thread* first = kernel().policy->pick_next();
            kernel().current[kickos_kernel_core()] = first;
            first->state = ThreadState::RUNNING;
            kernel().policy->on_switch_in(first);
            aspace_activate_for(first);
            first->mpu.apply();
#if KICKOS_LIBC_REENT
            // switch_book is not on this path: without this the FIRST thread would run on
            // libc's process-wide state until its first switch away and back.
            if (aspace_seated_for(first))
            {
                struct arch_aspace* const rspace = reent_space_of(first);
                if (first->reent_fresh)
                {
                    first->reent_fresh = false;
                    reent_prime(rspace, first->reent);
                }
                reent_seat(rspace, first->reent);
            }
#endif
            ktime_rearm();
            // arch_start does not return, so the bracket above is dropped by hand.
            klock_drop();
            arch_start(&kernel().boot[kickos_kernel_core()], &first->ctx);
        }

        void reschedule()
        {
            IrqLock lock;
            KICKOS_BENCH_MARK(bm_pick);
            Thread* next = kernel().policy->pick_next();
            KICKOS_BENCH_SPAN(PH_PICK_NEXT, bm_pick);
            uint32_t const cpu = arch_cpu_id();
            if (next == kernel().current[cpu])
            {
                return;
            }
            KICKOS_BENCH_MARK(bm_switch);
            switch_to(next);
            KICKOS_BENCH_SPAN(PH_SWITCH_TO, bm_switch);
        }

#if KICKOS_ARCH_HAS_IPC_FASTPATH
        struct arch_context* switch_prepare(Thread* next)
        {
            switch_book(next);
            return &next->ctx;
        }
#endif

        void yield()
        {
            IrqLock lock;
            kernel().policy->on_yield(current());
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
            kernel().policy->on_remove(current());
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
            // The unpark funnel, and so the one place a timed wait's deadline is dropped.
            // Never in wq_pop_highest: a CALL_SEND_WAIT caller popped there migrates park to
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
#if KICKOS_KERNEL_CORES > 1
            // Ahead of every refusal below: a core declining the switch still owes `t` a core.
            poke_peers_below(t->prio);
#endif
            Thread const* const c = current();
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
            // An RR slice expiry can rotate the dying thread off its ready-list head, and
            // pick_next would then take an equal-priority peer.
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
            // the old prio, change it, re-add at the new one. A bare t->prio write strands
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

        void exit_current(int code, ExitCause cause)
        {
            Thread* const c = kernel().current[kickos_kernel_core()];
#if KICKOS_KERNEL_STACKS
            // The only reader of the canary, and the only point where the thread that owns a
            // block is certainly finished with it. Armed at boot and never re-armed, so a
            // break here names an overflow that happened at any time since, on this slot.
            // NO DIAGNOSTIC PRINT HERE: this site is measured as the RET class, which binds
            // every thread's KICKOS_MIN_STACK_SIZE floor.
            int const kslot = kernel().threads.index_of(c);
#endif
#if KICKOS_KERNEL_STACKS
            if (kslot >= 0 and not kstack_canary_intact(kslot))
            {
                kpanic(diag::kKstackOverflow);
            }
#endif
            // Read by the WAIT_TASK_EMPTY sweep in the second locked block, past a
            // preemptible cap_teardown. `left_task` crosses that gap as a bare pointer, and
            // what keeps the compare unambiguous lives in task.cc: task_resolve refuses a
            // creator-less slot. Do not weaken that without revisiting this.
            Task* left_task = nullptr;
            bool emptied_task = false;
            {
                IrqLock lock;
                // Not EXITED yet: EXITED makes the slot reclaimable, and the sweep below runs
                // with interrupts unmasked between chunks. `state` cannot be the dying marker,
                // a switch back in rewrites it to RUNNING.
                c->dying = true;
                // Task-scoped death. MUST precede task_release, which may free the slot and
                // leave c->task a dangling name. A fault is the only death reaching the group
                // from here; every other death that wants the group has a caller who could name
                // it, and kos_task_kill and kos_task_slay cancel every member themselves.
                // CANCEL_KILL is cooperative: a peer keeps the window it holds long enough to
                // quiet its device.
                if (cause == EXIT_FAULTED)
                {
                    task_cancel_group(c->task, CANCEL_KILL);
                }
#if KICKOS_HAVE_ASPACE
                // The stack goes back BEFORE the task reference does: task_release below can
                // destroy the address space this stack is mapped into, and a space frees what
                // it maps, so a release after it would hand the pool frames the space had
                // already returned. Safe here because the descent is on this thread's own
                // kernel block, and nothing between here and the switch away touches the user
                // stack.
                if (c->kstack_owned)
                {
                    ustack_free(domain_space(task_domain(c->task)),
                                reinterpret_cast<uintptr_t>(c->stack_base), c->stack_size);
                    c->stack_base = nullptr;
                    c->stack_size = 0;
                    // The reclaim-point harvest reads this flag to push an arena block onto
                    // the free list.
                    c->kstack_owned = false;
                }
#endif
                // Before the sweep: its endpoint arm EPIPE-wakes a supervisor that may
                // respawn immediately, and the DEV-window exclusivity check (kernel.h)
                // refuses while a live thread holds the window; `dying` above is what takes
                // this one out of that scan. `left_task` is captured before the name is
                // retired below, a refcount read in the sweep being unable to say whether
                // THIS death emptied the group.
                left_task = c->task;
                emptied_task = task_release(c->task);
                // Retire the name with the reference. The slot may be free now and can be
                // re-handed in the sweep's first chunk gap; membership is a pointer
                // comparison, so a stale c->task makes this thread a phantom member of
                // whatever group lands there next.
                c->task = nullptr;
                // The creator's hold ends with the creator, keyed on the TAG: a recycled pool
                // slot answers kill_tag_of with its predecessor's, so a hold left behind is
                // creator authority its successor never earned.
                task_orphan_created_by(kernel().threads.kill_tag_of(c));
            }
            // Must close every cap the exiting thread holds BEFORE its slot is reclaimable,
            // else object references leak (destroy-on-last-close). Preemptible: it drops
            // IrqLock between chunks.
            cap_teardown(c);
            {
                IrqLock lock;
                Kernel& k = kernel();
                // THE BRACKET AROUND THIS STORE IS LOAD-BEARING: from here the slot reads
                // reclaimable to ThreadPool::alloc, and nothing between here and the swap may
                // release the kernel lock before kickos_switch_unlock parks this frame.
                c->state = ThreadState::EXITED;
                // The retry, and the only site that needs one: the reclaim already ran at
                // the note (cap.cc), and a refusal there means a live thread still held the
                // register window. Deferred while a concurrent sweep is in flight; the note
                // is sticky.
                if (not cap_teardown_active())
                {
                    console_on_driver_death();
                }
                k.policy->on_remove(c);
                if (c != k.idle[kickos_kernel_core()] and k.live > 0)
                {
                    k.live--;
                }
                // Join, wait-until-last and wait-for-the-group-to-empty park on no list, so
                // this pool scan IS the waiter lookup. The wait edge and wait_result are the
                // waker's to write BEFORE the wake. `state` is EXITED by now, which is the
                // clause in resched_after_wake that suppresses a switch from inside this loop.
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
                    // Only when THIS death emptied the group, compared by pointer as
                    // membership is.
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
            return kernel().current[kickos_kernel_core()];
        }
        Thread* idle()
        {
            return kernel().idle[kickos_kernel_core()];
        }
#if KICKOS_KERNEL_CORES > 1
        bool is_idle(Thread const* t)
        {
            for (uint32_t core = 0; core < KICKOS_KERNEL_CORES; core++)
            {
                if (kernel().idle[core] == t)
                {
                    return true;
                }
            }
            return false;
        }
#endif
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
            Thread* c = current();
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

#if KICKOS_KERNEL_CORES > 1
// The two halves of a peer core's arrival at the scheduler (arch/include/kickos/arch/arch.h).
// The cell read here is valid only on the far side of a nonzero kickos_kernel_core_seated.
extern "C" int kickos_kernel_core_ready(void)
{
    return ::kickos::kernel().idle[kickos_kernel_core()] != nullptr;
}

extern "C" void kickos_kernel_core_start(void)
{
    ::kickos::sched::start();
    while (true)
    {
        arch_idle_wait();
    }
}

// Runs in the doorbell's interrupt, so the switch this books is performed at the exception
// exit. A core still in bring-up reaches this before its scheduler exists.
extern "C" void kickos_kernel_core_resched(void)
{
    if (::kickos::kernel().current[kickos_kernel_core()] == nullptr)
    {
        return;
    }
    ::kickos::sched::reschedule();
}
#endif

// Reached only through a context arch_ctx_redirect rebuilt, so `current` IS the slain thread
// and this runs privileged at the top of its own stack.
extern "C" void kickos_thread_slay_exit(void* arg)
{
    (void)arg;
    ::kickos::sched::exit_current(KOS_EXIT_CANCELLED, ::kickos::sched::EXIT_RETURN);
}
