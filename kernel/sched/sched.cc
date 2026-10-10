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
#include <kickos/sync.h>
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

#if KICKOS_KERNEL_CORES > 1
#define SCHED_HOME_FOR(t, asker) ::kickos::sched_home_for((t), (asker))
#else
#define SCHED_HOME_FOR(t, asker) ((void)(t), (asker))
#endif

#if KICKOS_KERNEL_CORES > 1
        // Onto `home`'s own structure: sound for the calling core's, and for a core that has not
        // started (add_idle).
        void link_ready(Thread* t, uint32_t home)
        {
            KICKOS_DEBUG_ASSERT(sched_placeable_on(t, home));
            t->queue_core = static_cast<uint8_t>(home);
            t->rq_prio = t->prio;
            policy_on_ready(t);
        }

        void ring_stage(uint32_t to, uint16_t entry)
        {
            Kernel& k = kernel();
            uint32_t const me = kickos_kernel_core();
            uint32_t const s = k.sched_out[me].staged[to];
            KICKOS_DEBUG_ASSERT(s - k.sched_in[to].tail[me].load() < SCHED_RING_DEPTH);
            k.sched_slot[me][to][s & (SCHED_RING_DEPTH - 1u)] = entry;
            k.sched_out[me].staged[to] = s + 1u;
            k.sched_out[me].raise = k.sched_out[me].raise | (1u << to);
        }

        __attribute__((noinline)) void hand_to(Thread* t, uint32_t home)
        {
            Kernel& k = kernel();
            KICKOS_DEBUG_ASSERT(sched_placeable_on(t, home));
            KICKOS_DEBUG_ASSERT(k.threads.index_of(t) >= 0);
            t->queue_core = static_cast<uint8_t>(home);
            t->state.to<ThreadState::HANDED>();
            ring_stage(home, static_cast<uint16_t>(t - &k.threads.slots[0]));
            KICKOS_BENCH_SCHED_COUNT(::kickos::BS_HANDOFF);
        }

        // The one place a thread enters a ready structure, and only the calling core's, `me`: a
        // thread bound for a peer is HANDED and staged toward it, and that peer links it at its
        // drain. Inline, so a wake to its waker's core costs no frame beyond the link's.
        inline __attribute__((always_inline)) void publish_ready(Thread* t, uint32_t home,
                                                                  uint32_t me)
        {
            if (home == me)
            {
                link_ready(t, home);
                return;
            }
            hand_to(t, home);
        }

        // Linked on this core's own structure, so a request about it is applied here and now.
        inline bool linked_here(Thread const* t, uint32_t me)
        {
            return (t->state == ThreadState::READY or t->state == ThreadState::RUNNING)
                   and t->queue_core == me;
        }

        // A request that `t`'s priority, mask or claim be re-read by the core it is linked on or
        // headed to, for a thread not linked here: one linked on a peer, or handed toward one.
        // Any other re-reads the fields wherever it next moves. The request carries no value, and
        // the byte keeps a second one out of every ring until the first is retired.
        void ring_reseat(Thread* t, uint32_t me)
        {
            Kernel& k = kernel();
            bool const owed = t->state == ThreadState::READY or t->state == ThreadState::RUNNING
                              or (t->state == ThreadState::HANDED and t->queue_core != me);
            if (not owed)
            {
                return;
            }
            KICKOS_DEBUG_ASSERT(t->queue_core != me);
            KICKOS_DEBUG_ASSERT(k.threads.index_of(t) >= 0);
            if (t->reseat_owed != 0)
            {
                return;
            }
            t->reseat_owed = 1u;
            ring_stage(t->queue_core,
                       static_cast<uint16_t>(SCHED_ENTRY_RESEAT | (t - &k.threads.slots[0])));
            KICKOS_BENCH_RESEAT_ASKED(t);
        }

        void count_ask(uint32_t peers, [[maybe_unused]] uint8_t prio)
        {
            KOS_TRACE(::kickos::KOS_TR_ASK, peers, prio);
            KICKOS_BENCH_RESCHED_ASK(peers != 0);
        }

        // Moves READY `t`, linked here, to where the policy's placement invariant puts it.
        // Returns this core's bit when its next pass takes it: a peer `t` is staged toward is
        // raised by the flush that publishes the entry.
        uint32_t place(Thread* t, uint32_t me, Held held)
        {
            KICKOS_DEBUG_ASSERT(t->state == ThreadState::READY and t->queue_core == me);
            SchedPlacement const p = policy_place(t, sched_home_for(t, me), held);
            if (p.core != me)
            {
                policy_on_remove(t);
                publish_ready(t, p.core, me);
            }
            if (not p.below)
            {
                return 0;
            }
            if (p.core != me)
            {
                count_ask(1u << p.core, t->prio);
                return 0;
            }
            return 1u << me;
        }

        // For a thread made READY with no pass of this core to follow. A HANDED one is its
        // target's to place. Out of line: inlined, it deepens resched_after_wake's frame on the
        // trap chain.
        __attribute__((noinline)) void place_and_ask(Thread* t, uint32_t me, Held held)
        {
            if (t->state == ThreadState::HANDED)
            {
                return;
            }
            (void)place(t, me, held);
        }

        // `me` now runs below what it ran, so a peer may hold a thread `me` would take: every
        // started peer whose level exceeds this core's is asked, and re-reads this core's level
        // over its own structure before it moves anything.
        void drop_scan(uint32_t me, Held held)
        {
            Kernel& k = kernel();
            SchedOut& out = k.sched_out[me];
            int const mine = sched_effective_level(me, held);
            for (uint32_t h = 0; h < KICKOS_KERNEL_CORES; h++)
            {
                if (h == me or k.sched_out[h].level.load() == 0
                    or sched_effective_level(h, held) <= mine)
                {
                    continue;
                }
                out.drop_asked[h] = out.drop_asked[h].load() + 1u;
                out.raise = out.raise | (1u << h);
                count_ask(1u << h, static_cast<uint8_t>(mine + 1));
                KICKOS_BENCH_DROP_ASKED(h);
            }
        }

        // One thread per asking core, re-validated now: the asker's level may have risen since
        // it asked. The flush raises every core a thread was staged toward.
        void push_take(KernelCore me, Held held)
        {
            Kernel& k = kernel();
            SchedIn& in = k.sched_in[me];
            for (uint32_t c = 0; c < KICKOS_KERNEL_CORES; c++)
            {
                uint32_t const asked = k.sched_out[c].drop_asked[me].load();
                if (c == me or asked == in.drop_took[c].load())
                {
                    continue;
                }
                in.drop_took[c].store(asked);
                Thread* const t = policy_push_candidate(me, c, held);
                if (t == nullptr)
                {
                    KICKOS_BENCH_SCHED_COUNT(::kickos::BS_DROP_IDLE);
                    continue;
                }
                policy_on_remove(t);
                publish_ready(t, c, me);
                count_ask(1u << c, t->prio);
                KICKOS_BENCH_PUSHED(t, c);
            }
        }

        // A pass seating above what this core ran declines every READY thread here ranked above
        // it and at or below `next`: each expected this pass, and none was placed against `next`.
        void place_declined(Thread const* next, Thread const* woken, KernelCore me, Held held)
        {
            Kernel& k = kernel();
            SchedWalk walk{next->rq_prio + 1, nullptr};
            while (true)
            {
                Thread* const t = policy_declined(me, k.seated_prio[me], &walk);
                if (t == nullptr)
                {
                    return;
                }
                if (t != woken and t != next)
                {
                    (void)place(t, me, held);
                }
            }
        }

        inline void seat_level(Thread const* next, uint32_t me, Held held)
        {
            Kernel& k = kernel();
            if (next->rq_prio < k.seated_prio[me])
            {
                drop_scan(me, held);
            }
            k.seated_prio[me] = next->rq_prio;
        }

        // A thread whose mask no longer names this core is handed on unlinked. `place_it` is false
        // at start, whose own pass declines what it holds below the thread it seats.
        void ring_apply(uint16_t entry, KernelCore me, bool place_it, Held held)
        {
            Kernel& k = kernel();
            Thread* const t = &k.threads.slots[entry & ~SCHED_ENTRY_RESEAT];
            if ((entry & SCHED_ENTRY_RESEAT) != 0)
            {
                // Linked on a peer since the request: the byte kept any later request out of
                // every ring, so the entry follows the thread and keeps it. A thread still HANDED
                // is re-read by the link ahead of it, and a parked or dead one by what next
                // readies it.
                if (not linked_here(t, me))
                {
                    if (t->state == ThreadState::READY or t->state == ThreadState::RUNNING)
                    {
                        ring_stage(t->queue_core, entry);
                        return;
                    }
                    t->reseat_owed = 0;
                    KICKOS_BENCH_RESEAT_APPLIED(t, -1);
                    return;
                }
                t->reseat_owed = 0;
                uint8_t const old = t->rq_prio;
                if (t->prio != old)
                {
                    policy_on_remove(t);
                    t->rq_prio = t->prio;
                    policy_on_ready(t);
                }
                if (t->state == ThreadState::RUNNING)
                {
                    // A raise is never a drop; a lowering leaves the record high for the pass that
                    // follows, which also switches `t` away if its mask or claim now refuses it.
                    if (k.current(me) == t and t->rq_prio > old)
                    {
                        k.seated_prio[me] = t->rq_prio;
                    }
                    KICKOS_BENCH_RESEAT_APPLIED(t, 1);
                    return;
                }
                KICKOS_BENCH_RESEAT_APPLIED(t, 0);
                if (place_it)
                {
                    (void)place(t, me, held);
                }
                return;
            }
            KICKOS_DEBUG_ASSERT(t->state == ThreadState::HANDED and t->queue_core == me);
            if (not sched_placeable_on(t, me))
            {
                KICKOS_BENCH_SCHED_COUNT(::kickos::BS_HANDED_ON);
                publish_ready(t, sched_home_for(t, me), me);
                return;
            }
            t->state.to<ThreadState::READY>();
            link_ready(t, me);
            if (place_it)
            {
                (void)place(t, me, held);
            }
        }

        // Applies at most `budget` entries toward `me`, one per producer in turn from the cursor
        // and in order within a ring. Returns whether any is left.
        bool ring_drain(KernelCore me, uint32_t budget, bool place_it, Held held)
        {
            Kernel& k = kernel();
            SchedIn& in = k.sched_in[me];
            uint32_t p = in.cursor;
            uint32_t applied = 0;
            uint32_t dry = 0;
            while (dry < KICKOS_KERNEL_CORES)
            {
                uint32_t const from = p;
                p = p + 1u;
                if (p == KICKOS_KERNEL_CORES)
                {
                    p = 0;
                }
                uint32_t const tail = in.tail[from].load();
                if (from == me or tail == k.sched_out[from].head[me].load())
                {
                    dry++;
                    continue;
                }
                if (applied == budget)
                {
                    in.cursor = static_cast<uint8_t>(from);
                    return true;
                }
                uint16_t const entry = k.sched_slot[from][me][tail & (SCHED_RING_DEPTH - 1u)];
                // Retired before its thread is written, or a hand-on counts it twice.
                in.tail[from].store(tail + 1u);
                ring_apply(entry, me, place_it, held);
                KICKOS_BENCH_DRAIN_APPLIED();
                applied++;
                dry = 0;
            }
            in.cursor = static_cast<uint8_t>(p);
            return false;
        }
#else
        inline void publish_ready(Thread* t, uint32_t, uint32_t)
        {
            policy_on_ready(t);
        }
#endif

        inline __attribute__((always_inline)) void seat_running(Thread* next, KernelCore me)
        {
            kernel().current(me) = next;
            next->state.to<ThreadState::RUNNING>();
            policy_on_switch_in(next);
        }

#if KICKOS_LIBC_REENT
        // Only after the incoming memory view is installed, and only when that view belongs to
        // the incoming thread.
        inline __attribute__((always_inline)) void seat_reent(struct arch_aspace* rspace,
                                                              Thread* next)
        {
            if (aspace_seated_with(rspace))
            {
                if (next->reent_fresh)
                {
                    next->reent_fresh = false;
                    reent_prime(rspace, next->reent);
                }
#if not KICKOS_REENT_PER_THREAD
                reent_seat(rspace, next->reent);
#endif
            }
        }
#endif

        // prev is the published thread and may already differ from the executing one after a
        // deferred switch. Keep this inline for timing. Use the core ID only before
        // arch_switch; the thread may resume elsewhere.
        inline __attribute__((always_inline)) void switch_book(Thread* next, KernelCore me,
                                                               Held held)
        {
            KICKOS_BENCH_PERFORM_OPEN();
            KICKOS_BENCH_MARK(bm_book);
            Thread* prev = kernel().current(me);
#if KICKOS_KERNEL_CORES > 1
            Thread* displaced = nullptr;
#endif
            if (prev->state == ThreadState::RUNNING)
            {
                prev->state.to<ThreadState::READY>();
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
            seat_running(next, me);
            KOS_TRACE(::kickos::KOS_TR_RUN, KOS_TRACE_ID(next), next->prio);
            next->switch_count.store(next->switch_count.load() + 1u);
            KICKOS_BENCH_SPAN(PH_SWITCH_BOOK, bm_book);
            KICKOS_BENCH_MARK(bm_mpu);
            // What the incoming thread may touch is installed here and nowhere else.
            [[maybe_unused]] struct arch_aspace* const rspace = aspace_activate_for(next);
            next->mpu.apply();
            KICKOS_BENCH_SPAN(PH_MPU_APPLY, bm_mpu);
#if KICKOS_LIBC_REENT
            KICKOS_BENCH_MARK(bm_seat);
            seat_reent(rspace, next);
            KICKOS_BENCH_SPAN(PH_REENT_SEAT, bm_seat);
#endif
#if KICKOS_KERNEL_CORES > 1
            KICKOS_BENCH_DECIDE_OPEN();
            if (displaced != nullptr)
            {
                (void)place(displaced, me, held);
            }
            seat_level(next, me, held);
            KICKOS_BENCH_DECIDE_CLOSE();
#endif
            // Must arm for the incoming thread before the jump: nothing else programs its
            // policy deadline (RR slice).
            ktime_rearm(next, held);
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
            KernelCore const cpu = kickos_kernel_core();
            if (k.current(cpu) == nullptr)
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
            thread_cancel_kind(bad, CANCEL_SLAY, lock);
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
            if (k.current(cpu) == bad)
            {
                Thread* const next = policy_pick_next();
                if (next == nullptr)
                {
                    return nullptr;
                }
                switch_book(next, cpu, lock);
            }
            return &k.current(cpu)->ctx;
        }

        void switch_to(Thread* next, KernelCore me, Held held)
        {
            Thread* prev = kernel().current(me);
            switch_book(next, me, held);
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
        void pick_and_seat(Thread* woken, Held held)
        {
            KernelCore const me = kickos_kernel_core();
#if KICKOS_KERNEL_CORES > 1
            KICKOS_BENCH_PASS_OPEN();
#endif
            KICKOS_BENCH_MARK(bm_pick);
            Thread* next = policy_pick_next();
            KICKOS_BENCH_SPAN(PH_PICK_NEXT, bm_pick);
#if KICKOS_KERNEL_CORES > 1
            if (woken != nullptr and next != woken and woken->state != ThreadState::HANDED)
            {
                (void)place(woken, me, held);
            }
            if (next->rq_prio > kernel().seated_prio[me])
            {
                place_declined(next, woken, me, held);
            }
            if (next == kernel().current(me))
            {
                seat_level(next, me, held);
                KICKOS_BENCH_PASS_CLOSE();
                return;
            }
#else
            (void)woken;
            if (next == kernel().current(me))
            {
                return;
            }
#endif
            KICKOS_BENCH_MARK(bm_switch);
            switch_to(next, me, held);
            KICKOS_BENCH_SPAN(PH_SWITCH_TO, bm_switch);
        }
    }

#if KICKOS_KERNEL_CORES > 1
    // Exact under the kernel lock, which every producer stages under and every core changes its
    // bitmap under. The TCB and not the entry is read, a later request being free to move `prio`
    // while the entry is in flight.
    int sched_effective_level(uint32_t core, Held)
    {
        Kernel const& k = kernel();
        int lvl = static_cast<int>(k.sched_out[core].level.load()) - 1;
        for (uint32_t p = 0; p < KICKOS_KERNEL_CORES; p++)
        {
            uint32_t const end = k.sched_out[p].staged[core];
            for (uint32_t i = k.sched_in[core].tail[p].load(); i != end; i++)
            {
                uint16_t const e = k.sched_slot[p][core][i & (SCHED_RING_DEPTH - 1u)];
                Thread const* const x = &k.threads.slots[e & ~SCHED_ENTRY_RESEAT];
                if (x->queue_core == core and x->prio > lvl
                    and (x->state == ThreadState::HANDED or x->state == ThreadState::READY
                         or x->state == ThreadState::RUNNING))
                {
                    lvl = x->prio;
                }
            }
        }
        return lvl;
    }

    void sched_flush_owed(uint32_t me)
    {
        SchedOut& out = kernel().sched_out[me];
        uint32_t const targets = out.raise;
        out.raise = 0;
        for (uint32_t to = 0; to < KICKOS_KERNEL_CORES; to++)
        {
            if ((targets & (1u << to)) != 0)
            {
                out.head[to].store(out.staged[to]);
            }
        }
        klock_resched_ask(targets);
    }
#endif

    namespace sched
    {

        void init()
        {
            // No ready-structure reset here: policy-owned, and zeroed with the BSS instance.
            Kernel& k = kernel();
            KernelCore const me = kickos_kernel_core();
            k.current(me) = nullptr;
            k.idle(me) = nullptr;
            k.live = 0;
        }

        void add(Thread* t, [[maybe_unused]] Held held)
        {
            KICKOS_ASSERT_EXCLUSION_HELD();
            KernelCore const me = kickos_kernel_core();
            t->state.to<ThreadState::READY>();
            if (t->prio == KICKOS_PRIO_IDLE)
            {
#if KICKOS_KERNEL_CORES > 1
                // Set before publishing: the boot core's idle is seated by the same rule
                // add_idle uses for a peer's, and publish_ready asserts the home is inside it.
                t->affinity = 1u << me;
#endif
                publish_ready(t, me, me);
                kernel().idle(me) = t;
            }
            else
            {
                publish_ready(t, SCHED_HOME_FOR(t, me), me);
                kernel().live++;
#if KICKOS_KERNEL_CORES > 1
                // A thread handed to a peer is placed by that peer's drain.
                place_and_ask(t, me, held);
#endif
            }
        }

#if KICKOS_KERNEL_CORES > 1
        void add_idle(Thread* t, KernelCore core)
        {
            IrqLock lock;
            t->state.to<ThreadState::READY>();
            // Exactly this core's bit. pick_next's unconditional idle fallback, not this, is
            // what guarantees an isolated core still has an idle thread; this only keeps idle
            // inside the affinity invariant every reader of the field relies on.
            t->affinity = 1u << core;
            // The one sanctioned cross-home link, sound because the target core has not started:
            // nothing there can be picking while this writes. Idle is outside the pool, so no
            // ring can name it.
            link_ready(t, core);
            kernel().idle(core) = t;
        }
#endif

#if KICKOS_KERNEL_CORES > 1
        void set_affinity(Thread* t, uint32_t mask)
        {
            IrqLock lock;
            KernelCore const me = kickos_kernel_core();
            if (t->affinity == mask)
            {
                return;
            }
            t->affinity = mask;
            if (not linked_here(t, me))
            {
                ring_reseat(t, me);
                return;
            }
            // klock_resched_ask cannot reach the caller's own core, so a thread this core must
            // take or give up is moved by this core's pass, taken here.
            if (t->state == ThreadState::READY)
            {
                if (place(t, me, lock) == (1u << me))
                {
                    reschedule(nullptr, lock);
                }
                return;
            }
            // RUNNING is published with the current seat of the core running it.
            [[maybe_unused]] Thread const* const seated = kernel().current(me);
            KICKOS_DEBUG_ASSERT(seated == t);
            if (not sched_placeable_on(t, me))
            {
                reschedule(nullptr, lock);
            }
        }

        void reseat(Thread* t, Held)
        {
            KICKOS_ASSERT_EXCLUSION_HELD();
            uint32_t const me = kickos_kernel_core();
            if (linked_here(t, me))
            {
                klock_resched_self();
                return;
            }
            ring_reseat(t, me);
        }
#endif

        void start()
        {
            IrqLock lock;
            KernelCore const me = kickos_kernel_core();
#if KICKOS_KERNEL_CORES > 1
            // Everything handed here before this core started, once per boot.
            (void)ring_drain(me, UINT32_MAX, false, lock);
#endif
            Thread* first = policy_pick_next();
            seat_running(first, me);
#if KICKOS_KERNEL_CORES > 1
            policy_on_start(me);
#endif
            [[maybe_unused]] struct arch_aspace* const rspace = aspace_activate_for(first);
            first->mpu.apply();
#if KICKOS_LIBC_REENT
            seat_reent(rspace, first);
#endif
            ktime_rearm(first, lock);
#if KICKOS_KERNEL_CORES > 1
            // A core that starts declines what it holds below `first`, and its level falls from
            // nothing to `first`.
            place_declined(first, nullptr, me, lock);
            kernel().seated_prio[me] = first->rq_prio;
            drop_scan(me, lock);
#endif
            // arch_start does not return, so the bracket above is dropped by hand.
            klock_drop();
#if KICKOS_KERNEL_CORES > 1
            sched_flush(me);
#endif
            KICKOS_BENCH_LOCK_DROP();
            arch_start(&kernel().boot(me), &first->ctx);
        }

        void reschedule(Thread* woken, Held held)
        {
            KICKOS_ASSERT_EXCLUSION_HELD();
            pick_and_seat(woken, held);
        }

#if KICKOS_ARCH_HAS_IPC_FASTPATH
        struct arch_context* switch_prepare(Thread* next, Held held)
        {
            KICKOS_ASSERT_EXCLUSION_HELD();
            switch_book(next, kickos_kernel_core(), held);
            return &next->ctx;
        }
#endif

        void yield()
        {
            IrqLock lock;
            policy_on_yield(current());
            reschedule(nullptr, lock);
        }

        void detach_current(Held)
        {
            KICKOS_ASSERT_EXCLUSION_HELD();
            // Blocking is legal only from thread context: from an ISR the switch defers and
            // the supposedly blocked thread keeps running.
            if (arch_in_isr())
            {
                kpanic(diag::kBlockInIsr);
            }
            policy_on_remove(current());
        }

        bool wake_no_resched(Thread* t, Held held)
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
                ktime_deadline_cancel(t, held);
            }
            // Every other state must be refused: EXITED is what ThreadPool::alloc reads as a
            // free slot, so readying an exited thread takes that slot out of the pool for good.
            if (t->state != ThreadState::BLOCKED)
            {
                KOS_TRACE(::kickos::KOS_TR_REFUSED, KOS_TRACE_ID(t),
                          static_cast<uint32_t>(static_cast<ThreadState>(t->state)));
                KICKOS_BENCH_SPAN(PH_WAKE_UNPARK, bm_unpark);
                return false;
            }
            t->state.to<ThreadState::READY>();
            // The wait edge is what confers this write: the waker owns a parked thread's
            // control block, so the waker is who chooses its owner, and choosing its own core
            // is what makes a wake land where the waker runs.
            uint32_t const me = kickos_kernel_core();
            publish_ready(t, SCHED_HOME_FOR(t, me), me);
            KOS_TRACE(::kickos::KOS_TR_READY, KOS_TRACE_ID(t), t->prio);
            KICKOS_BENCH_SPAN(PH_WAKE_UNPARK, bm_unpark);
            return true;
        }

        void resched_after_wake(Thread* t, Held held)
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
                place_and_ask(t, kickos_kernel_core(), held);
#endif
                return;
            }
            pick_and_seat(t, held);
        }

#if KICKOS_KERNEL_CORES > 1
        void place_ready(Thread* t, Held held)
        {
            KICKOS_ASSERT_EXCLUSION_HELD();
            place_and_ask(t, kickos_kernel_core(), held);
        }
#endif

        void wake(Thread* t, Held held)
        {
            KICKOS_ASSERT_EXCLUSION_HELD();
            if (wake_no_resched(t, held))
            {
                resched_after_wake(t, held);
            }
        }

        void set_prio(Thread* t, uint8_t p, [[maybe_unused]] Held held)
        {
            KICKOS_ASSERT_EXCLUSION_HELD();
            if (t->prio == p)
            {
                return;
            }
#if KICKOS_KERNEL_CORES > 1
            Kernel& k = kernel();
            KernelCore const me = kickos_kernel_core();
            if (not linked_here(t, me))
            {
                t->prio = p;
                ring_reseat(t, me);
                return;
            }
            uint8_t const old = t->rq_prio;
            policy_on_remove(t);
            t->prio = p;
            t->rq_prio = p;
            policy_on_ready(t);
            if (t->state == ThreadState::READY)
            {
                // A caller mid-walk cannot take a pass, so this core owes itself one.
                if (place(t, me, held) == (1u << me))
                {
                    klock_resched_self();
                }
                return;
            }
            // A raise is never a drop. A lowering leaves seated_prio high for the pass that sees
            // it, which the caller takes.
            if (k.current(me) == t and p > old)
            {
                k.seated_prio[me] = p;
            }
#else
            if (t->state == ThreadState::READY or t->state == ThreadState::RUNNING)
            {
                policy_on_remove(t);
                t->prio = p;
                policy_on_ready(t);
                return;
            }
            // A BLOCKED thread is on no ready list, and neither queue it can be on is
            // prio-ordered: wq_pop_highest rescans at pop, the timer list is deadline-sorted.
            t->prio = p;
#endif
        }

        void exit_current(int code, ExitCause cause, IrqLock* held)
        {
            Thread* const c = kernel().current(kickos_kernel_core());
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
            uint16_t left_gen = 0;
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
                // A fault ends the group (task_end), and so does the entry's death by any cause.
                // A cancelled thread latches nothing: the task keeps KOS_EXIT_CANCELLED.
                if (cause == EXIT_FAULTED or c->task_entry)
                {
                    task_end(c->task, code, c->cancel_kind == CANCEL_NONE, lock);
                }
#if KICKOS_HAVE_ASPACE
#if KICKOS_PRESYNC
                // A thread leaving inside a call's work outside the lock: the run it staged has
                // no other owner.
                presync_exit();
#endif
                // Unmap this thread's windows, the shootdown included, before the release below
                // frees a device for a second holder: a sibling keeps the space and must not
                // keep the mapping.
                aspace_window_unmap_holder(
                    domain_space(thread_domain(c)), domain_ranges_mut(thread_domain(c)),
                    static_cast<uint16_t>(kernel().threads.index_of(c) + 1));
                // Release the user stack before task_release can destroy its space.
                // Execution is on the kernel stack; no later code may touch the user stack.
                if (c->kstack_owned)
                {
                    ustack_free(thread_domain(c),
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
                // Release the device windows before teardown can wake a supervisor into a
                // respawn that asks for them. No return to user code follows.
                c->mpu.drop_devices();
#if KICKOS_ARCH_HAS_PORTS
                // Its ports too: the next switch on this core closes them.
                c->ctx.port_count = 0;
                c->ctx.port_places = 0;
#endif
                // Capture task identity before retiring membership.
                left_task = c->task;
                left_gen = task_gen(c->task);
                emptied_task = task_release(c->task, lock);
                // Clear the task pointer if this death empties the group, since its slot
                // may be freed during teardown gaps. With surviving siblings, retain it
                // until teardown finishes so still-open capabilities remain in the task budget.
                if (emptied_task)
                {
                    c->task = nullptr;
                }
                // The creator's hold ends with the creator, keyed on the tag: a recycled pool
                // slot answers kill_tag_of with its predecessor's, so a hold left behind is
                // creator authority its successor never earned.
                task_orphan_created_by(kernel().threads.kill_tag_of(c), lock);
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
                c->state.to<ThreadState::EXITED>();
                // Root's slot is never recycled, so release its empty capability directory here.
                if (k.threads.is_root(c))
                {
                    thread_cap_release(c);
                }
                // Retry deferred console reclaim after device holders exit and concurrent
                // sweeps complete.
                if (not cap_teardown_active())
                {
                    console_on_driver_death(lock);
                }
                policy_on_remove(c);
                if (c != k.idle(kickos_kernel_core()) and k.live > 0)
                {
                    k.live--;
                }
                // By the last member to finish its sweep, so a slay it wakes and a restart the
                // report prompts find everything the task's threads held already free.
                bool const task_dead = task_sweep_done(left_task, left_gen);
                // EXITED suppresses switching while this loop scans ThreadPool for join and
                // task-empty waiters.
                for (int s = 0; s < k.threads.next; s++)
                {
                    Thread* const w = &k.threads.slots[s];
                    // A task-empty waiter only once the group is empty and swept, compared by
                    // pointer as membership is. The creator's watch is raised below the loop.
                    if (w->wait_join_target() == c
                        or (task_dead and w->wait_task_target() == left_task))
                    {
                        thread_abort_park(w, 0, lock);
                    }
                }
                if (task_dead)
                {
                    task_report_death(left_task, left_gen);
                }
                if (k.live == 0)
                {
                    kickos_terminate(code);
                }
                reschedule(nullptr, lock);
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
            return kernel().current(kickos_kernel_core());
        }
        Thread* idle()
        {
            return kernel().idle(kickos_kernel_core());
        }
#if KICKOS_KERNEL_CORES > 1
        bool is_idle(Thread const* t)
        {
            for (KernelCore const core : KernelCores::all())
            {
                if (kernel().idle(core) == t)
                {
                    return true;
                }
            }
            return false;
        }
#endif
        uint64_t next_timed_event(Thread const* t)
        {
            return policy_next_timed_event(t);
        }

        void tick_rr(uint64_t now, Held held)
        {
            KICKOS_ASSERT_EXCLUSION_HELD();
            Thread* c = current();
            if (c == nullptr)
            {
                return;
            }
            if (now < policy_next_timed_event(c))
            {
                return;
            }
            policy_on_slice_expire(c);
            reschedule(nullptr, held);
        }

    }
}

#if KICKOS_KERNEL_CORES > 1
// Runs in the doorbell's interrupt, so the switch this books is performed at the exception
// exit. A core still in bring-up reaches this before its scheduler exists.
extern "C" void kickos_kernel_core_resched_if_owed(void)
{
    if (not ::kickos::klock_resched_take())
    {
        return;
    }
#if KICKOS_BENCH_SCHED_ON
    ::kickos::BenchReasonScope const bench_reason(::kickos::BR_RESCHED);
#endif
    // The doorbell handler masks interrupts but still needs IrqLock on SMP.
    ::kickos::IrqLock lock;
    ::kickos::KernelCore const me = kickos_kernel_core();
    if (::kickos::kernel().current(me) == nullptr)
    {
        return;
    }
    // The drain lives here and nowhere a pass can be mid-walk, and what it leaves is owed to
    // this core's next dispatch.
    KICKOS_BENCH_DRAIN_OPEN();
    bool const over = ::kickos::ring_drain(me, ::kickos::SCHED_DRAIN_BUDGET, true, lock);
    KICKOS_BENCH_DRAIN_CLOSE();
    if (over)
    {
        KICKOS_BENCH_SCHED_COUNT(::kickos::BS_DRAIN_OVER);
        ::kickos::klock_resched_self();
    }
    // A drop ask always carries a raise to its holder, so the dispatch that raise enters is
    // where it is answered, and no ordinary pass reads the drop cells.
    ::kickos::push_take(me, lock);
    ::kickos::sched::reschedule(nullptr, lock);
}
#endif

// Reached only through a context arch_ctx_redirect rebuilt, so `current` is the slain thread
// and this runs privileged at the top of its own stack.
extern "C" void kickos_thread_slay_exit(void* arg)
{
    (void)arg;
#if KICKOS_KERNEL_STACKS
    // The stub completes the death on any stack, so this is the only witness that it runs
    // where a domain sibling cannot rewrite its frames.
    KICKOS_ASSERT(::kickos::sched::current()->ctx.kernel_sp == 0
                  or kickos_fault_frame_on_kernel_stack(__builtin_stack_address(), 0));
#endif
    ::kickos::krecord_abandon();
    ::kickos::sched::exit_current(KOS_EXIT_CANCELLED, ::kickos::sched::EXIT_RETURN);
}
