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

    // THE ONE ADMISSION OF A CORE MASK. Every path that seats one goes through it: a spawn, a
    // re-affinity, and both halves of a task's grant.
    //
    // The machine first, then the grant: a bit naming a core this kernel does not schedule
    // cannot be refused merely for being set, which is what keeps all ones an ordinary
    // request. Naming NO core it schedules is the -KOS_EINVAL, a request no grant could
    // satisfy.
    //
    // `requested` is a REAL MASK here, empty being malformed. A zero word means something
    // different at each ABI that carries one (the task's default set at spawn and at
    // kos_thread_set_affinity, leave this half alone in a grant), so each resolves its own
    // before calling.
    //
    // TWO CLAUSES SUFFICE, so no placeability test belongs here: what survives is a NON-EMPTY
    // subset of KICKOS_CORE_SET_ALL, which is exactly bits [0, KICKOS_KERNEL_CORES), and every
    // one of those bits names a core pick_next scans.
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

    // THE EXCLUSION, AND HOW EVERY DECLARATION BELOW IS READ AGAINST IT.
    //
    // A function marked "caller holds the exclusion" runs with it ALREADY HELD: an IrqLock the
    // caller constructed, or the mask a trap or doorbell body was entered with, which IS the
    // exclusion on a kernel configured to one core. Above one core that mask alone is NOT
    // enough, the cross-core kernel lock being the other half, so a masked-context caller
    // there constructs the object like anyone else.
    //
    // EVERY DECLARATION FALLS IN ONE OF THREE CLASSES AND THE NOTE IS WHAT NAMES IT, so a
    // call site is settled by reading the one declaration it names and nothing further.
    //   HELD      carries the note, and never acquires. Most of this file.
    //   ACQUIRES  constructs its own IrqLock. start, yield, add_idle, set_affinity and
    //             exit_current, which are the whole of the class.
    //   NEITHER   init, set_policy, default_policy, current, idle, is_idle, live_count and
    //             next_timed_event, which install or read one pointer or word.
    //
    // TWO OF THE FIVE ARE REACHED BOTH WAYS, SO "NOTHING RE-ACQUIRES WHAT ITS CALLER HOLDS"
    // IS NOT THE RULE AND AN AUDIT MADE ON IT MISREADS THEM. IrqLock nests in both halves
    // (kickos/irqlock.h): each instance restores the interrupt state it found, and the kernel
    // lock is taken and released as the per-core depth crosses zero. What each is reached by:
    //   start         boot alone, holding nothing, and does not return.
    //   yield         the yield syscall arm, a zero-length sleep, and the console-publish
    //                 drain, which leaves the exclusion around each pass on purpose. All three
    //                 hold nothing.
    //   add_idle      bring-up alone, holding nothing. Its sibling `add` is HELD, and the same
    //                 boot path brackets that one by hand.
    //   set_affinity  HOLDING IT from the routed-core pin (kernel/irq/irq.cc) and from the
    //                 kos_thread_set_affinity arm, both of which resolve and authority-check
    //                 under their own bracket; holding nothing from the bench harness.
    //   exit_current  holding nothing from the return trampoline, the fault stub, the slay
    //                 trampoline, the kos_exit arm, and the cancel check syscall_dispatch
    //                 makes on syscall entry, which is a death point reached with no bracket
    //                 at all (tests/static/check_park_death_point.sh states why it reads
    //                 there); HOLDING IT from every park prologue whose park_cancel_pending
    //                 finds a cancel. It never returns, so such a caller's bracket is
    //                 abandoned rather than destroyed and the depth leaves this core with the
    //                 frame, not with that object.
    //
    // AND AN ABANDONED BRACKET BREAKS A PRECONDITION exit_current ITSELF HONOURS. cap_teardown
    // (kernel/syscall/cap.cc) declares that its caller must NOT hold IrqLock: it drops and
    // retakes one every chunk so that a sweep as wide as the capability table stays
    // preemptible, and above one core those gaps are also where the cross-core lock is
    // released. exit_current calls it outside its own brackets and satisfies that. A caller
    // that reached exit_current holding one does not: the nested retakes restore the masked
    // state they found, the per-core depth never returns to zero, and the whole sweep runs
    // masked with the kernel lock held. Every park prologue above is such a caller. That is a
    // known defect and not the intent; it is recorded in TODO.md under the M8.8 review
    // residue, with what closing it takes.
    //
    // The one form is prose, as it is for cap_resolve and switch_prepare, and above one core a
    // debug build checks it at the body: KICKOS_ASSERT_EXCLUSION_HELD (kickos/klock.h) reads
    // the per-core lock state the caller's IrqLock already maintains, so nothing stands beside
    // the lock as a second truth. AT ONE KERNEL CORE IT CHECKS NOTHING, the exclusion
    // there being the interrupt mask and no seam reporting it, so a green single-core run
    // witnesses no bracket at all and the note is the whole of what holds.
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
        // Re-place `t` onto `mask`. The caller has already intersected it with t's task's core
        // set and found it non-empty, so this writes and re-places rather than judging.
        //
        // A READY, BLOCKED or newly created thread just becomes eligible elsewhere at the next
        // pick. A thread EXECUTING on a core the new mask excludes is not yanked: it is made
        // ineligible there and that core is asked to reschedule, which is the only way off.
        void set_affinity(Thread* t, uint32_t mask);
#endif

#if KICKOS_KERNEL_CORES > 1
        // sched::add for a thread that is to be core `core`'s idle fallback.
        void add_idle(Thread* t, uint32_t core);
#endif

        // Enter the first thread from the boot context. Does not return: the
        // scheduler ends the process via arch_shutdown (never unwinds to boot).
        void start();

        // The bare entry to the single decision point, which is pick_and_seat. Safe to call
        // from thread or ISR context; caller holds the exclusion.
        //
        // `woken` names a thread this pass readied and owes a core to. Above one core a pick
        // that DECLINES it is the only thing that can announce it to a peer, at ITS priority
        // and not the caller's, so a pass that readied somebody and passes nullptr strands a
        // thread pinned elsewhere. Null where the pass readied nobody.
        void reschedule(Thread const* woken = nullptr);

        // Voluntary yield: rotate within priority, then reschedule. ACQUIRES, and the
        // console-publish drain calls it from outside a bracket it left on purpose.
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

        // A reschedule leaves `current` naming the woken thread while this caller still
        // runs, so a caller that reads sched::current() again after waking must ready with
        // wake_no_resched (true iff it readied t) and defer one resched_after_wake, for the
        // HIGHEST-priority thread it woke, to every path that does not itself park.
        // Both hold the exclusion as their precondition.
        //
        // THAT DEFERRAL COVERS THE LOCAL SEAT AND NOTHING ELSE. A caller readying several
        // threads still owes announce_ready for each of the others: see below.
        [[nodiscard]] bool wake_no_resched(Thread* t);
        void resched_after_wake(Thread const* t);

#if KICKOS_KERNEL_CORES > 1
        // Tell the peers that could take READY thread `t` to look, at ITS priority, with no
        // local pick and no switch. One seat can hold only one thread, so a caller readying
        // several and deferring ONE reschedule reaches every peer for one of them and none
        // for the rest, and a thread whose affinity excludes this core is then reachable by
        // nothing at all. Announcing costs no local decision, so it need not be deferred.
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

        // The SOLE writer of a thread's effective priority: no other code may write
        // t->prio. A READY or RUNNING thread is re-seated through the policy hooks, since
        // rq_remove locates its list by reading t->prio and a bare field write would
        // corrupt the ready lists; a BLOCKED thread takes the value directly,
        // because wait queues scan lazily at pop and the timer list is prio-independent.
        // Does NOT reschedule; the caller decides. NOT BOUNDED BY THE TASK'S PRIORITY
        // CEILING: every caller here is priority inheritance or the console-publish temporary,
        // which are the kernel's own and not a task asking for priority. The ceiling is
        // enforced where a task asks, at the spawn boundary. Caller holds the exclusion, and
        // the console-publish drain re-enters it around each of its two calls rather than
        // spanning the wait between them.
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
