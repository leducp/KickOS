// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The scheduler core. reschedule() is the single point where a context switch
// is decided; every trigger, including the tick, funnels through it.

#ifndef KICKOS_SCHED_H
#define KICKOS_SCHED_H

#include <stdint.h>

#include <kickos/thread.h>

#include <kickos/sys/errno.h>
#include <kickos/held.h>

namespace kickos
{
    class IrqLock;
    enum KernelCore : uint32_t;

#if KICKOS_KERNEL_CORES > 1
    // The placement rule: `t->affinity` already carries every narrowing applied to the thread,
    // so its task's core set is not re-read here. This makes the scheduler anti-work-conserving
    // by construction: a runnable thread waits while a core it may not run on idles.
    inline bool sched_placeable_on(Thread const* t, uint32_t core)
    {
        return (t->affinity & (1u << core)) != 0;
    }

    // The core a thread is published to when nothing better is known: the asker if the mask
    // admits it, else the lowest core the mask names. A pinned thread has one owner for life.
    // An empty mask answers the asker; publish_ready's assert names the caller. The loop avoids
    // a count-trailing-zeros builtin, which lowers to a libgcc helper on rv64imac.
    inline uint32_t sched_home_for(Thread const* t, uint32_t asker)
    {
        if (sched_placeable_on(t, asker))
        {
            return asker;
        }
        for (uint32_t core = 0; core < KICKOS_KERNEL_CORES; core++)
        {
            if (sched_placeable_on(t, core))
            {
                return core;
            }
        }
        return asker;
    }

    // Where the policy puts a READY thread: `core` is the structure it belongs on, and `below`
    // says that core's next pass takes it.
    struct SchedPlacement
    {
        uint32_t core;
        bool below;
    };

    // Where a walk of one core's pick order resumes: at `next`, or at the head of the highest
    // list below `below` when `next` is null.
    struct SchedWalk
    {
        int below;
        Thread* next;
    };

    // How a grant bounds a request. A thread's affinity is a set of acceptable cores, so the
    // grant intersects it and all ones is the identity of that intersection rather than a
    // magic value. A task's grant is an authority: it narrows only, and a request reaching
    // past it is refused and never clamped.
    enum class MaskBound : uint8_t
    {
        INTERSECT,
        SUBSET
    };

    // Validates masks for spawn, affinity changes, and task grants. Out-of-machine bits are
    // ignored rather than rejected. Callers must resolve ABI-specific zero-mask defaults
    // before calling: zero here is invalid.
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

    // The FIFO + round-robin policy (kernel/sched/policy_fifo_rr.cc) owns which thread runs next
    // and the ready structure; the core touches ready state only through these.
    Thread* policy_pick_next();
    void policy_on_ready(Thread* t);
    void policy_on_remove(Thread* t);
    void policy_on_yield(Thread* t);
    void policy_on_slice_expire(Thread* t);

    // The core owns the clock, the policy the deadline. on_switch_in arms the incoming thread;
    // next_timed_event is its earliest deadline (UINT64_MAX = none), answered for the thread named
    // rather than the current one: the switch path knows the incoming thread before it is seated.
    void policy_on_switch_in(Thread* t);
    uint64_t policy_next_timed_event(Thread const* t);

#if KICKOS_KERNEL_CORES > 1
    // Cross-core placement, answered over the policy's own ready structure. `place` names where
    // READY `t` belongs, `home` standing for the core it is on or is about to be published to.
    // `push_candidate` names a READY thread on `holder` that `target`'s next pass would take and
    // `holder`'s would not, or nullptr. `declined` names, in pick order from `walk`, a READY
    // thread on `core` ranking strictly between `above` and `walk->below` that some other core
    // may run, or nullptr, and leaves `walk` at the thread after it: between calls the caller may
    // move the answer off `core` and must move nothing else there.
    SchedPlacement policy_place(Thread const* t, uint32_t home, Held held);
    Thread* policy_push_candidate(KernelCore holder, uint32_t target, Held held);
    Thread* policy_declined(KernelCore core, int above, SchedWalk* walk);
    // `core` has seated its first thread: until now peers read it as not started.
    void policy_on_start(KernelCore core);

    // What `core`'s next pass seats if nothing more arrives: its published level raised by every
    // thread already on its way there, or -1 before it starts.
    int sched_effective_level(uint32_t core, Held held);
#endif

    // An entry taking a Held requires interrupt masking and, on SMP, the kernel lock. start,
    // yield, add_idle, set_affinity and exit_current take their own IrqLock, which nests;
    // exit_current also ends the caller's when handed it.
    //
    // Debug builds also check the exclusion on SMP. Single-core builds cannot: the
    // architecture has no interrupt-mask query interface.
    namespace sched
    {
        void init();

        // Register a fully-initialized thread as READY.
        void add(Thread* t, Held held);

#if KICKOS_KERNEL_CORES > 1
        // Set an already-validated nonempty affinity mask. A READY thread linked here is placed
        // again at once, a blocked one at its next wake. A thread linked on a peer, or handed
        // toward one, is re-read by that peer's dispatch, whose pass moves a running one off it.
        void set_affinity(Thread* t, uint32_t mask);

        // Asks the core `t` is linked on or headed to for a pass that re-reads its priority, mask
        // and claim: this core's own pass when `t` is linked here, else that core's dispatch.
        void reseat(Thread* t, Held held);

        // sched::add for a thread that is to be core `core`'s idle fallback.
        void add_idle(Thread* t, KernelCore core);
#endif

        // Enter the first thread from the boot context. Does not return: the
        // scheduler ends the process via arch_shutdown (never unwinds to boot).
        void start();

        // Reschedule in thread or ISR context. Pass the thread made ready as woken: above one
        // core, a pass that does not take it places it. Pass nullptr only when no thread was
        // made ready.
        void reschedule(Thread* woken, Held held);

        // Yield within this priority and reschedule. Takes IrqLock internally.
        void yield();

        // Remove `current` from the ready list without rescheduling. A blocking
        // primitive must call this before parking the thread on a wait queue,
        // since the ready list and wait queues share the TCB link node.
        void detach_current(Held held);

        // Make a previously-removed thread runnable again; preempts if warranted.
        void wake(Thread* t, Held held);

        // wake_no_resched returns true if it made t ready, without changing current().
        // Defer resched_after_wake until current() is no longer needed, using the
        // highest-priority thread woken. Place other woken threads separately.
        [[nodiscard]] bool wake_no_resched(Thread* t, Held held);
        void resched_after_wake(Thread* t, Held held);

#if KICKOS_KERNEL_CORES > 1
        // Place READY t across cores and ask the core that takes it, without a local pass.
        // Required for each thread made ready that is not passed to the deferred reschedule.
        void place_ready(Thread* t, Held held);
#endif

#if KICKOS_ARCH_HAS_IPC_FASTPATH
        // The bookkeeping half of switch_to, for the IPC fastpath, which runs inside the
        // trap handler and performs the register swap itself instead of pending one. Every
        // state change switch_to makes happens here; what is left out is arch_switch.
        // Returns the incoming context for the arch epilogue to restore.
        struct arch_context* switch_prepare(Thread* next, Held held);
#endif

        // Only this function may write effective priority. Reinsert READY/RUNNING
        // threads through policy hooks before changing prio, which indexes their lists.
        // BLOCKED threads need no reorder because wait queues scan priorities at pop.
        // Does not reschedule the calling core or apply the task priority ceiling; inherited
        // priority is kernel-controlled. Above one core a READY thread linked here is placed
        // again, and a thread linked on a peer, or handed toward one, is re-seated by that
        // peer's dispatch.
        void set_prio(Thread* t, uint8_t p, Held held);

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
        //
        // `held` is the caller's OUTERMOST bracket, or null for a caller holding none. It is
        // ended here, once the thread is marked dying and before the capability sweep, which
        // drops the exclusion between chunks and so is preemptible only with no bracket
        // beneath it.
        void exit_current(int code, ExitCause cause, IrqLock* held = nullptr)
            __attribute__((noreturn));

        Thread* current();
        Thread* idle();

        // Whether `t` is an idle thread on any core; idle() answers for the calling core alone.
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

        // The policy's earliest timed event for `t` (ns), or UINT64_MAX for none. `t` may be null.
        uint64_t next_timed_event(Thread const* t);
        // Runs in the timer ISR on every expiry: if the policy has a timed event due at `now`
        // (an RR slice), let it act, then reschedule. The expiry body takes the exclusion before
        // reading the queue.
        void tick_rr(uint64_t now, Held held);
    }
}

#endif
