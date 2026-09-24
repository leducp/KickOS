// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The ready structure is policy-owned, never this file's.

#include <kickos/aspace.h>
#include <kickos/notify.h>
#include <kickos/sched.h>
#include <kickos/smptrace.h>
#include <kickos/bench.h>
#include <kickos/cap.h>
#include <kickos/console_tx.h>
#include <kickos/debug.h>
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

// The home at one core is the only core, and the derivation is not compiled there at all.
#if KICKOS_KERNEL_CORES > 1
#define SCHED_HOME_FOR(t, asker) ::kickos::sched_home_for((t), (asker))
#else
#define SCHED_HOME_FOR(t, asker) ((void)(t), (asker))
#endif

#if KICKOS_KERNEL_CORES > 1
        // The one place a thread enters a ready structure. A publish to another core's structure
        // is sound only under the kernel lock, made only by the thread's current holder or its
        // waker: no core takes a thread out of a peer's structure.
        void publish_ready(Thread* t, uint32_t home)
        {
            KICKOS_DEBUG_ASSERT(sched_placeable_on(t, home));
            t->queue_core = static_cast<uint8_t>(home);
            kernel().policy->on_ready(t);
        }

        // Every helper below returns the cores it owes an ask, and the caller raises them in
        // one klock_resched_ask: an ask raised from deeper down a pass sits deeper on the trap
        // chain that pass runs on.
        void count_ask(uint32_t peers, [[maybe_unused]] uint8_t prio)
        {
            KOS_TRACE(::kickos::KOS_TR_ASK, peers, prio);
            KICKOS_BENCH_RESCHED_ASK(peers != 0);
        }

        // klock_resched_ask cannot reach the caller's own core.
        inline __attribute__((always_inline)) void ask(uint32_t cores, uint32_t me)
        {
            cores &= ~(1u << me);
            if (cores != 0)
            {
                klock_resched_ask(cores);
            }
        }

        // Moves READY `t` to where the policy's placement invariant puts it. Returns the bit of
        // the core whose next pass takes it, `me`'s included, or 0.
        uint32_t place(Thread* t, uint32_t me)
        {
            KICKOS_DEBUG_ASSERT(t->state == ThreadState::READY);
            uint32_t home = t->queue_core;
            if (not sched_placeable_on(t, home))
            {
                home = sched_home_for(t, me);
            }
            SchedPlacement const p = kernel().policy->place(t, home);
            if (p.core != t->queue_core)
            {
                kernel().policy->on_remove(t);
                publish_ready(t, p.core);
            }
            uint32_t taker = 0;
            if (p.below)
            {
                taker = 1u << p.core;
            }
            count_ask(taker & ~(1u << me), t->prio);
            return taker;
        }

        // For a thread made READY with no pass of this core to follow. Out of line: inlined, it
        // deepens resched_after_wake's frame on the trap chain.
        __attribute__((noinline)) void place_and_ask(Thread* t, uint32_t me)
        {
            ask(place(t, me), me);
        }

        // `me` now runs below what it ran, so a peer may hold a thread `me` would take. The
        // holder pushes it: a request per holder, carried by the holder's doorbell.
        uint32_t drop_scan(uint32_t me)
        {
            Kernel& k = kernel();
            uint32_t holders = 0;
            for (uint32_t h = 0; h < KICKOS_KERNEL_CORES; h++)
            {
                if (h == me or k.current[h] == nullptr)
                {
                    continue;
                }
                Thread const* const t = k.policy->push_candidate(h, me);
                if (t == nullptr)
                {
                    continue;
                }
                k.push_asked[h] |= 1u << me;
                holders |= 1u << h;
                count_ask(1u << h, t->prio);
            }
            return holders;
        }

        // One thread per asking core, re-validated now: the asker's level may have risen since
        // it asked.
        uint32_t push_take(uint32_t me)
        {
            Kernel& k = kernel();
            uint32_t const asked = k.push_asked[me];
            k.push_asked[me] = 0;
            uint32_t targets = 0;
            for (uint32_t c = 0; c < KICKOS_KERNEL_CORES; c++)
            {
                if ((asked & (1u << c)) == 0)
                {
                    continue;
                }
                Thread* const t = k.policy->push_candidate(me, c);
                if (t == nullptr)
                {
                    continue;
                }
                k.policy->on_remove(t);
                publish_ready(t, c);
                targets |= 1u << c;
                count_ask(1u << c, t->prio);
            }
            return targets;
        }

        // A pass seating above what this core ran declines every READY thread here ranked above
        // it and at or below `next`: each expected this pass, and none was placed against `next`.
        uint32_t place_declined(Thread const* next, Thread const* woken, uint32_t me)
        {
            Kernel& k = kernel();
            uint32_t takers = 0;
            SchedWalk walk{next->prio + 1, nullptr};
            while (true)
            {
                Thread* const t = k.policy->declined(me, k.seated_prio[me], &walk);
                if (t == nullptr)
                {
                    return takers;
                }
                if (t != woken and t != next)
                {
                    takers |= place(t, me);
                }
            }
        }

        // The drop test every pass makes on its way out, whichever thread it seated.
        inline uint32_t seat_level(Thread const* next, uint32_t me)
        {
            Kernel& k = kernel();
            uint32_t holders = 0;
            if (next->prio < k.seated_prio[me])
            {
                holders = drop_scan(me);
            }
            k.seated_prio[me] = next->prio;
            return holders;
        }
#else
        inline void publish_ready(Thread* t, uint32_t)
        {
            kernel().policy->on_ready(t);
        }
#endif

        // Caller holds IrqLock. prev is the published thread and may already differ
        // from the executing one after a deferred switch. Keep this inline for timing.
        // Use the core ID only before arch_switch; the thread may resume elsewhere.
        inline __attribute__((always_inline)) void switch_book(Thread* next, uint32_t me)
        {
            KICKOS_BENCH_MARK(bm_book);
            Thread* prev = kernel().current[me];
#if KICKOS_KERNEL_CORES > 1
            Thread* displaced = nullptr;
#endif
            if (prev->state == ThreadState::RUNNING)
            {
                prev->state = ThreadState::READY;
#if KICKOS_KERNEL_CORES > 1
                // The first moment a running thread can move: until this store it IS running,
                // and a mask that narrowed under it is honoured here. A slain thread stays for
                // the self-owed pass that claims it, unless its mask no longer admits this core.
                bool const slain = thread_slay_claim_pending(prev);
                if ((prev->affinity & ~(1u << me)) != 0
                    and (not slain or not sched_placeable_on(prev, me)))
                {
                    displaced = prev;
                }
                if (slain)
                {
                    klock_resched_self();
                }
#endif
            }
            kernel().current[me] = next;
            next->state = ThreadState::RUNNING;
            KOS_TRACE(::kickos::KOS_TR_RUN, KOS_TRACE_ID(next), next->prio);
            next->switch_count.store(next->switch_count.load() + 1u);
            kernel().policy->on_switch_in(next);
            KICKOS_BENCH_SPAN(PH_SWITCH_BOOK, bm_book);
            KICKOS_BENCH_MARK(bm_mpu);
            // What the incoming thread may touch is installed here and nowhere else.
            [[maybe_unused]] struct arch_aspace* const rspace = aspace_activate_for(next);
            next->mpu.apply();
            KICKOS_BENCH_SPAN(PH_MPU_APPLY, bm_mpu);
#if KICKOS_LIBC_REENT
            KICKOS_BENCH_MARK(bm_seat);
            // Write libc state only after installing the incoming memory view, and only
            // when that view belongs to the incoming thread.
            if (aspace_seated_with(rspace))
            {
                if (next->reent_fresh)
                {
                    next->reent_fresh = false;
                    reent_prime(rspace, next->reent);
                }
                reent_seat(rspace, next->reent);
            }
            KICKOS_BENCH_SPAN(PH_REENT_SEAT, bm_seat);
#endif
#if KICKOS_KERNEL_CORES > 1
            uint32_t asks = 0;
            if (displaced != nullptr)
            {
                asks = place(displaced, me);
            }
            ask(asks | seat_level(next, me), me);
#endif
            // Must arm for the incoming thread before the jump: nothing else programs its
            // policy deadline (RR slice).
            ktime_rearm(next);
            // Redirect only the incoming context before arch_switch; deferred switches
            // overwrite the outgoing context with live registers. dying prevents
            // restarting cap_teardown after it releases IrqLock between chunks.
            if (thread_slay_claim_pending(next))
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
            uint32_t const cpu = kickos_kernel_core();
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
            // Privileged wild pointers are kernel bugs, not containable thread faults.
            // Idle is outside ThreadPool and has already been excluded.
            if (bad->privileged)
            {
                return nullptr;
            }
            thread_cancel_kind(bad, CANCEL_SLAY);
            // Contain only after slay succeeds; switch_book must rebuild the stale context.
            if (not thread_slay_claim_pending(bad))
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
                switch_book(next, cpu);
            }
            return &k.current[cpu]->ctx;
        }

        void switch_to(Thread* next, uint32_t me)
        {
            Thread* prev = kernel().current[me];
            switch_book(next, me);
            // Hold the kernel lock until the outgoing frame is saved.
            // The thread carries its nesting depth across the switch.
            uint32_t const klock_depth = klock_detach();
            // The bench bracket's depth rides this frame for the same reason the lock's does.
            // Its start does not: that one describes the core's masked window, which the
            // thread resuming here continues.
            KICKOS_BENCH_LOCK_DETACH(bench_depth);
            KICKOS_BENCH_MARK(bm_arch);
            arch_switch(&prev->ctx, &next->ctx);
            KICKOS_BENCH_SPAN(PH_ARCH_SWITCH, bm_arch);
            KICKOS_BENCH_LOCK_ATTACH(bench_depth);
            klock_attach(klock_depth);
        }

        // Above one core a pass places what it made READY and did not take: the woken thread
        // and the ones it declined here, the displaced one in switch_book. A parked or exiting
        // thread is placed by whatever readies it next.
        void pick_and_seat(Thread* woken)
        {
            uint32_t const me = kickos_kernel_core();
#if KICKOS_KERNEL_CORES > 1
            uint32_t asks = 0;
            if (kernel().push_asked[me] != 0)
            {
                asks = push_take(me);
            }
#endif
            KICKOS_BENCH_MARK(bm_pick);
            Thread* next = kernel().policy->pick_next();
            KICKOS_BENCH_SPAN(PH_PICK_NEXT, bm_pick);
#if KICKOS_KERNEL_CORES > 1
            if (woken != nullptr and next != woken)
            {
                asks |= place(woken, me);
            }
            if (next->prio > kernel().seated_prio[me])
            {
                asks |= place_declined(next, woken, me);
            }
            if (next == kernel().current[me])
            {
                ask(asks | seat_level(next, me), me);
                return;
            }
            ask(asks, me);
#else
            (void)woken;
            if (next == kernel().current[me])
            {
                return;
            }
#endif
            KICKOS_BENCH_MARK(bm_switch);
            switch_to(next, me);
            KICKOS_BENCH_SPAN(PH_SWITCH_TO, bm_switch);
        }
    }

    namespace sched
    {

        void init()
        {
            // No ready-structure reset here: policy-owned, and zeroed with the BSS instance.
            Kernel& k = kernel();
            uint32_t const me = kickos_kernel_core();
            k.current[me] = nullptr;
            k.idle[me] = nullptr;
            k.live = 0;
            k.policy = default_policy();
        }

        void set_policy(SchedPolicy const* policy)
        {
            kernel().policy = policy;
        }

        void add(Thread* t)
        {
            KICKOS_ASSERT_EXCLUSION_HELD();
            uint32_t const me = kickos_kernel_core();
            t->state = ThreadState::READY;
            if (t->prio == KICKOS_PRIO_IDLE)
            {
#if KICKOS_KERNEL_CORES > 1
                // Set before publishing: the boot core's idle is seated by the same rule
                // add_idle uses for a peer's, and publish_ready asserts the home is inside it.
                t->affinity = 1u << me;
#endif
                publish_ready(t, me);
                kernel().idle[me] = t;
            }
            else
            {
                publish_ready(t, SCHED_HOME_FOR(t, me));
                kernel().live++;
#if KICKOS_KERNEL_CORES > 1
                place_and_ask(t, me);
#endif
            }
        }

#if KICKOS_KERNEL_CORES > 1
        void add_idle(Thread* t, uint32_t core)
        {
            IrqLock lock;
            t->state = ThreadState::READY;
            // Exactly this core's bit. pick_next's unconditional idle fallback, not this, is
            // what guarantees an isolated core still has an idle thread; this only keeps idle
            // inside the affinity invariant every reader of the field relies on.
            t->affinity = 1u << core;
            // The one sanctioned cross-home publication, sound because the target core has not
            // started: nothing there can be picking while this writes.
            publish_ready(t, core);
            kernel().idle[core] = t;
        }
#endif

#if KICKOS_KERNEL_CORES > 1
        void set_affinity(Thread* t, uint32_t mask)
        {
            IrqLock lock;
            uint32_t const me = kickos_kernel_core();
            if (t->affinity == mask)
            {
                return;
            }
            t->affinity = mask;
            // klock_resched_ask cannot reach the caller's own core, so a thread this core must
            // take or give up is moved by this core's pass, taken here.
            if (t->state != ThreadState::RUNNING)
            {
                if (t->state == ThreadState::READY)
                {
                    uint32_t const taker = place(t, me);
                    ask(taker, me);
                    if (taker == (1u << me))
                    {
                        reschedule();
                    }
                }
                return;
            }
            // A RUNNING thread the new mask still admits keeps the core it is on.
            for (uint32_t core = 0; core < KICKOS_KERNEL_CORES; core++)
            {
                if (kernel().current[core] != t)
                {
                    continue;
                }
                if (sched_placeable_on(t, core))
                {
                    return;
                }
                if (core == me)
                {
                    reschedule();
                    return;
                }
                klock_resched_ask(1u << core);
                return;
            }
            // RUNNING is published with the current[] seat of the core running it, so this is
            // reached only with the two disagreeing.
            KICKOS_DEBUG_ASSERT(t->state != ThreadState::RUNNING);
        }
#endif

        void start()
        {
            IrqLock lock;
            uint32_t const me = kickos_kernel_core();
            Thread* first = kernel().policy->pick_next();
            kernel().current[me] = first;
            first->state = ThreadState::RUNNING;
            kernel().policy->on_switch_in(first);
            [[maybe_unused]] struct arch_aspace* const rspace = aspace_activate_for(first);
            first->mpu.apply();
#if KICKOS_LIBC_REENT
            // switch_book is not on this path: without this the first thread would run on
            // libc's process-wide state until its first switch away and back.
            if (aspace_seated_with(rspace))
            {
                if (first->reent_fresh)
                {
                    first->reent_fresh = false;
                    reent_prime(rspace, first->reent);
                }
                reent_seat(rspace, first->reent);
            }
#endif
            ktime_rearm(first);
#if KICKOS_KERNEL_CORES > 1
            // A core that starts declines what it holds below `first`, and its level falls from
            // nothing to `first`.
            uint32_t const asks = place_declined(first, nullptr, me);
            kernel().seated_prio[me] = first->prio;
            ask(asks | drop_scan(me), me);
#endif
            // arch_start does not return, so the bracket above is dropped by hand.
            klock_drop();
            KICKOS_BENCH_LOCK_DROP();
            arch_start(&kernel().boot[me], &first->ctx);
        }

        void reschedule(Thread* woken)
        {
            KICKOS_ASSERT_EXCLUSION_HELD();
            pick_and_seat(woken);
        }

#if KICKOS_ARCH_HAS_IPC_FASTPATH
        struct arch_context* switch_prepare(Thread* next)
        {
            KICKOS_ASSERT_EXCLUSION_HELD();
            switch_book(next, kickos_kernel_core());
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
            KICKOS_ASSERT_EXCLUSION_HELD();
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
            KICKOS_ASSERT_EXCLUSION_HELD();
            // Caller must already have set current->state and linked it onto its queue.
            // Timer path only: sleepq uses the separate tnext link. A wait-queue caller
            // shares the ready/wait link node and must detach before linking (wq_block).
            detach_current();
            reschedule();
        }

        bool wake_no_resched(Thread* t)
        {
            KICKOS_ASSERT_EXCLUSION_HELD();
            // The refusals below do no ready-queue work and still feed this row, so its
            // minimum can be a refusal rather than a readying.
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
                KOS_TRACE(::kickos::KOS_TR_REFUSED, KOS_TRACE_ID(t),
                          static_cast<uint32_t>(t->state));
                KICKOS_BENCH_SPAN(PH_WAKE_UNPARK, bm_unpark);
                return false;
            }
            t->state = ThreadState::READY;
            // The wait edge is what confers this write: the waker owns a parked thread's
            // control block, so the waker is who chooses its owner, and choosing its own core
            // is what makes a wake land where the waker runs.
            publish_ready(t, SCHED_HOME_FOR(t, kickos_kernel_core()));
            KOS_TRACE(::kickos::KOS_TR_READY, KOS_TRACE_ID(t), t->prio);
            KICKOS_BENCH_SPAN(PH_WAKE_UNPARK, bm_unpark);
            return true;
        }

        void resched_after_wake(Thread* t)
        {
            KICKOS_ASSERT_EXCLUSION_HELD();
            // Do not switch before start or during exit_current cleanup.
            // An equal-priority wake must not rotate an exiting thread away either.
            Thread const* const c = current();
            if (c == nullptr or c->state == ThreadState::EXITED
                or (c->dying and t->prio <= c->prio))
            {
#if KICKOS_KERNEL_CORES > 1
                // Declined without a pass, so placed here; an exiting caller's own pass takes
                // it where it lands on this core.
                place_and_ask(t, kickos_kernel_core());
#endif
                return;
            }
            pick_and_seat(t);
        }

#if KICKOS_KERNEL_CORES > 1
        void place_ready(Thread* t)
        {
            KICKOS_ASSERT_EXCLUSION_HELD();
            place_and_ask(t, kickos_kernel_core());
        }
#endif

        void wake(Thread* t)
        {
            KICKOS_ASSERT_EXCLUSION_HELD();
            if (wake_no_resched(t))
            {
                resched_after_wake(t);
            }
        }

        void set_prio(Thread* t, uint8_t p)
        {
            KICKOS_ASSERT_EXCLUSION_HELD();
            if (t->prio == p)
            {
                return;
            }
            // The caller handles rescheduling this core.
            if (t->state == ThreadState::READY or t->state == ThreadState::RUNNING)
            {
#if KICKOS_KERNEL_CORES > 1
                uint8_t const old = t->prio;
#endif
                kernel().policy->on_remove(t);
                t->prio = p;
                kernel().policy->on_ready(t);
#if KICKOS_KERNEL_CORES > 1
                Kernel& k = kernel();
                uint32_t const me = kickos_kernel_core();
                if (t->state == ThreadState::READY)
                {
                    uint32_t const taker = place(t, me);
                    ask(taker, me);
                    // A caller mid-walk cannot take a pass, so this core owes itself one.
                    if (taker == (1u << me))
                    {
                        klock_resched_self();
                    }
                    return;
                }
                uint32_t const core = t->queue_core;
                if (k.current[core] != t)
                {
                    return;
                }
                // A raise is never a drop. A lowering leaves seated_prio high for the pass
                // that sees it, which on this core the caller takes.
                if (p > old)
                {
                    k.seated_prio[core] = p;
                }
                else if (core != me)
                {
                    klock_resched_ask(1u << core);
                }
#endif
                return;
            }
            // A BLOCKED thread is on no ready list, and neither queue it can be on is
            // prio-ordered: wq_pop_highest rescans at pop, the timer list is deadline-sorted.
            t->prio = p;
        }

        void exit_current(int code, ExitCause cause, IrqLock* held)
        {
            Thread* const c = kernel().current[kickos_kernel_core()];
#if KICKOS_KERNEL_STACKS
            // Check the stack canary only after the thread has finished; do not print here,
            // since this exit path sets the minimum stack size.
            int const kslot = kernel().threads.index_of(c);
            if (kslot >= 0 and not kstack_canary_intact(kslot))
            {
                kpanic(diag::kKstackOverflow);
            }
#endif
            // Save the task pointer across preemptible teardown. Identity remains valid
            // because task_resolve rejects slots without a creator.
            Task* left_task = nullptr;
            bool emptied_task = false;
            {
                IrqLock lock;
                // Not EXITED yet: EXITED makes the slot reclaimable, and the sweep below runs
                // with interrupts unmasked between chunks. `state` cannot be the dying marker,
                // a switch back in rewrites it to RUNNING.
                c->dying = true;
                // Before the sweep, not inside it: cap_teardown drops and retakes IrqLock
                // every KCAP_TEARDOWN_CHUNK slots, and a notification still naming this thread
                // across one of those gaps is a wake into a thread already in teardown.
                // Unconsumed bits are left pending for the next server.
                notify_unbind_self(c);
                // Cancel faulted-task peers before releasing the task slot. Use cooperative
                // cancellation so device holders can shut down their hardware.
                if (cause == EXIT_FAULTED)
                {
                    task_cancel_group(c->task, CANCEL_KILL);
                }
#if KICKOS_HAVE_ASPACE
                // Release the user stack before task_release can destroy its space.
                // Execution is on the kernel stack; no later code may touch the user stack.
                if (c->kstack_owned)
                {
                    ustack_free(task_domain(c->task),
                                reinterpret_cast<uintptr_t>(c->stack_base), c->stack_size);
                    c->stack_base = nullptr;
                    c->stack_size = 0;
                    // The reclaim-point harvest reads this flag to push an arena block onto
                    // the free list.
                    c->kstack_owned = false;
                }
#if KICKOS_KERNEL_CORES > 1
                aspace_install_boot();
#endif
#endif
                // Release device-window ownership before teardown can wake a supervisor.
                // Capture task identity before retiring membership.
                left_task = c->task;
                emptied_task = task_release(c->task);
                // Clear the task pointer if this death empties the group, since its slot
                // may be reused during teardown gaps. With surviving siblings, retain it
                // until teardown finishes so still-open capabilities remain in the task budget.
                if (emptied_task)
                {
                    c->task = nullptr;
                }
                // The creator's hold ends with the creator, keyed on the tag: a recycled pool
                // slot answers kill_tag_of with its predecessor's, so a hold left behind is
                // creator authority its successor never earned.
                task_orphan_created_by(kernel().threads.kill_tag_of(c));
            }
            // Ended only past `dying`, so a cancel read under the caller's bracket reaches the
            // mark with the exclusion never lapsing.
            if (held != nullptr)
            {
                held->end();
            }
            // Must close every cap the exiting thread holds before its slot is reclaimable,
            // else object references leak (destroy-on-last-close).
            cap_teardown(c);
            {
                IrqLock lock;
                // The sweep is done, so no capability of this thread is seated any more and
                // the name has nothing left to account for. Retiring it here rather than
                // above is what keeps a sibling bounded across the sweep.
                c->task = nullptr;
                Kernel& k = kernel();
                // Keep the kernel lock from publishing EXITED until the switch saves this frame.
                c->state = ThreadState::EXITED;
                // Root's slot is never recycled, so release its empty capability directory here.
                if (k.threads.is_root(c))
                {
                    thread_cap_release(c);
                }
                // Retry deferred console reclaim after device holders exit and concurrent
                // sweeps complete.
                if (not cap_teardown_active())
                {
                    console_on_driver_death();
                }
                k.policy->on_remove(c);
                if (c != k.idle[kickos_kernel_core()] and k.live > 0)
                {
                    k.live--;
                }
                // EXITED suppresses switching while this loop scans ThreadPool for join and
                // task-empty waiters.
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
                    // Only when this death emptied the group, compared by pointer as
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

        uint64_t next_timed_event(Thread const* t)
        {
            return kernel().policy->next_timed_event(t);
        }

        void tick_rr(uint64_t now)
        {
            KICKOS_ASSERT_EXCLUSION_HELD();
            Thread* c = current();
            if (c == nullptr)
            {
                return;
            }
            // A policy with no timed event reports UINT64_MAX.
            if (now < kernel().policy->next_timed_event(c))
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
    // The doorbell handler masks interrupts but still needs IrqLock on SMP.
    ::kickos::IrqLock lock;
    if (::kickos::kernel().current[kickos_kernel_core()] == nullptr)
    {
        return;
    }
    ::kickos::sched::reschedule();
}
#endif

// Reached only through a context arch_ctx_redirect rebuilt, so `current` is the slain thread
// and this runs privileged at the top of its own stack.
extern "C" void kickos_thread_slay_exit(void* arg)
{
    (void)arg;
    ::kickos::sched::exit_current(KOS_EXIT_CANCELLED, ::kickos::sched::EXIT_RETURN);
}
