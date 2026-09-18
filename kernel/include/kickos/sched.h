// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The scheduler core. reschedule() is the single point where a context switch
// is decided (invariant #2); every trigger funnels through it. The tick is not
// special. A pluggable policy (RTEMS-style) decides pick_next; FIFO+RR ship.

#ifndef KICKOS_SCHED_H
#define KICKOS_SCHED_H

#include <stdint.h>

#include <kickos/thread.h>

#include <kickos/sys/errno.h>

namespace kickos
{
#if KICKOS_KERNEL_CORES > 1
    // THE PLACEMENT RULE, and the only one. `t->affinity` already carries every narrowing
    // applied to the thread, so its task's core set is not re-read here.
    //
    // ANTI-WORK-CONSERVING BY CONSTRUCTION: a runnable thread waits while a core it may not run
    // on idles. That is the feature.
    inline bool sched_placeable_on(Thread const* t, uint32_t core)
    {
        return (t->affinity & (1u << core)) != 0;
    }

    // How a grant bounds a request. A THREAD's affinity is a set of acceptable cores, so the
    // grant intersects it and all ones is the identity of that intersection rather than a
    // magic value. A TASK's grant is an authority: it narrows only, and a request reaching
    // past it is refused and never clamped.
    enum class MaskBound : uint8_t
    {
        INTERSECT,
        SUBSET
    };

    // Validate masks for spawn, affinity changes, and task grants.
    // First intersect requested with the machine's core set, then check the grant.
    // An empty machine intersection returns EINVAL. Out-of-machine bits are ignored.
    // Callers must resolve ABI-specific zero-mask defaults before calling; zero
    // here is invalid. Every accepted bit names a core the scheduler can use.
    inline int sched_admit_mask(uint32_t requested, uint32_t grant, MaskBound bound,
                                uint32_t* effective)
    {
        uint32_t const wanted = requested & KICKOS_CORE_SET_ALL;
        if (wanted == 0)
        {
            return -KOS_EINVAL;
        }
        uint32_t const admitted = wanted & grant;
        if (bound == MaskBound::SUBSET and admitted != wanted)
        {
            return -KOS_EPERM;
        }
        if (admitted == 0)
        {
            return -KOS_EPERM;
        }
        *effective = admitted;
        return 0;
    }
#endif

    // Pluggable scheduling policy. The core is pure mechanism (run state, current, the
    // context switch); the policy owns WHICH thread runs next and holds the ready
    // structure. The core must never touch ready state except through these hooks.
    struct SchedPolicy
    {
        // Scheduling decision.
        Thread* (*pick_next)();           // highest-priority runnable thread
        void (*on_ready)(Thread*);        // enqueue a now-runnable thread
        void (*on_remove)(Thread*);       // dequeue a thread leaving the run set
        void (*on_yield)(Thread*);        // current voluntarily yielded
        void (*on_slice_expire)(Thread*); // the running thread's timed slice elapsed

        // Timed-event seam (RR today): the core owns the clock, the policy the
        // deadline. on_switch_in arms the incoming thread; next_timed_event is the
        // earliest policy deadline for the tickless timer (UINT64_MAX = none), answered for
        // the thread NAMED rather than for whatever currently[] holds: the switch path knows
        // the incoming thread before it is seated.
        void (*on_switch_in)(Thread*);
        uint64_t (*next_timed_event)(Thread const*);
    };

    // Scheduler locking rules:
    // - "Caller holds the exclusion" requires interrupt masking and, on SMP, the
    //   kernel lock. An IrqLock provides both.
    // - start, yield, add_idle, set_affinity, and exit_current take their own IrqLock.
    // - init, set_policy, default_policy, current, idle, is_idle, live_count, and
    //   next_timed_event do not acquire it.
    //
    // IrqLock can nest. set_affinity and exit_current also accept locked callers.
    // Known limitation: exit_current reached from a locked park prologue keeps the
    // outer lock through cap_teardown, preventing its intended preemption gaps.
    // See the M8.8 review residue in TODO.md.
    //
    // Debug builds check held-lock preconditions on SMP. Single-core builds cannot
    // check them because the architecture has no interrupt-mask query interface.
    namespace sched
    {
        void init();
        void set_policy(SchedPolicy const* policy);

        // The built-in FIFO + round-robin policy (kernel/sched/policy_fifo_rr.cc);
        // installed by init(). Other policies swap in via set_policy().
        SchedPolicy const* default_policy();

        // Register a fully-initialized thread as READY. Caller holds the exclusion.
        void add(Thread* t);

#if KICKOS_KERNEL_CORES > 1
        // Set an already-validated nonempty affinity mask. Ready or blocked threads
        // become eligible on the new cores at their next scheduling decision.
        // If a running thread is excluded from its current core, request a reschedule.
        void set_affinity(Thread* t, uint32_t mask);
#endif

#if KICKOS_KERNEL_CORES > 1
        // sched::add for a thread that is to be core `core`'s idle fallback.
        void add_idle(Thread* t, uint32_t core);
#endif

        // Enter the first thread from the boot context. Does not return: the
        // scheduler ends the process via arch_shutdown (never unwinds to boot).
        void start();

        // Reschedule in thread or ISR context. Caller holds the exclusion.
        // Pass the thread made ready as woken so an SMP peer can run it if this core
        // does not. Pass nullptr only when no thread was made ready.
        void reschedule(Thread const* woken = nullptr);

        // Yield within this priority and reschedule. Takes IrqLock internally.
        void yield();

        // Remove `current` from the run set (state must already be set to the reason,
        // e.g. BLOCKED), then reschedule. Returns when the thread is resumed. Caller holds the
        // exclusion.
        void block_current();

        // Remove `current` from the ready list WITHOUT rescheduling. A blocking
        // primitive must call this BEFORE parking the thread on a wait queue,
        // since the ready list and wait queues share the TCB link node. Caller holds the
        // exclusion.
        void detach_current();

        // Make a previously-removed thread runnable again; preempts if warranted. Caller
        // holds the exclusion.
        void wake(Thread* t);

        // wake_no_resched returns true if it made t ready, without changing current().
        // Defer resched_after_wake until current() is no longer needed, using the
        // highest-priority thread woken. Announce other woken threads separately.
        // Both functions require the caller to hold the exclusion.
        [[nodiscard]] bool wake_no_resched(Thread* t);
        void resched_after_wake(Thread const* t);

#if KICKOS_KERNEL_CORES > 1
        // Notify eligible peers that t is ready, using t's priority. Does not switch
        // or make a local scheduling decision. Required for each thread made ready
        // that is not passed to the deferred reschedule.
        void announce_ready(Thread const* t);
#endif

#if KICKOS_ARCH_HAS_IPC_FASTPATH
        // The bookkeeping half of switch_to, for the IPC fastpath, which runs inside the
        // trap handler and performs the register swap itself instead of pending one. Every
        // state change switch_to makes happens here; what is left out is arch_switch.
        // Returns the incoming context for the arch epilogue to restore. Caller holds
        // IrqLock (the fastpath is entered with interrupts already masked by the trap).
        struct arch_context* switch_prepare(Thread* next);
#endif

        // Only this function may write effective priority. Reinsert READY/RUNNING
        // threads through policy hooks before changing prio, which indexes their lists.
        // BLOCKED threads need no reorder because wait queues scan priorities at pop.
        // Does not reschedule or apply the task priority ceiling; inherited priority
        // is kernel-controlled. Caller holds the exclusion.
        void set_prio(Thread* t, uint8_t p);

        // What this death is, which is the one thing exit_current cannot derive: a fault
        // carries no cancel_kind, so a contained fault and an ordinary return arrive
        // indistinguishable and scope the death differently.
        enum ExitCause : uint8_t
        {
            EXIT_RETURN = 0, // an ordinary return, kos_exit, or a cancel honoured at a death point
            EXIT_FAULTED = 1 // a fault the arch reporter contained (kernel/init/fault.cc)
        };

        // Terminate the current thread with exit code `code`; never returns. The
        // code is used only if this is the last non-idle thread (it ends the
        // process); otherwise the thread just leaves the run set.
        void exit_current(int code, ExitCause cause) __attribute__((noreturn));

        Thread* current();
        Thread* idle();

        // Whether `t` is an idle thread on ANY core; idle() answers for the calling core alone.
#if KICKOS_KERNEL_CORES > 1
        bool is_idle(Thread const* t);
#else
        inline bool is_idle(Thread const* t)
        {
            return t == idle();
        }
#endif

        // Live non-idle thread count (0 => nothing left to run).
        unsigned live_count();

        // The active policy's earliest timed event for `t` (ns), or UINT64_MAX for none.
        // Consumed by the time subsystem when arming the tickless timer (RR slice
        // expiry today; the core carries no notion of a "slice"). `t` may be null.
        uint64_t next_timed_event(Thread const* t);
        // Runs in the timer ISR on every expiry: if the active policy has a
        // timed event due at `now` (an RR slice), let it act, then reschedule. Caller holds
        // the exclusion; the expiry body takes it before reading the queue.
        void tick_rr(uint64_t now);
    }
}

#endif
