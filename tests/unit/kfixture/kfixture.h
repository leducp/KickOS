// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Host fixture for the kernel's own state machines: a real Kernel instance, the real
// scheduler, the real FIFO/RR policy, the real capability teardown, the real task pool, with
// karch_seam.cc standing in for the arch boundary.
//
// Keep this header GTEST-FREE; kseam_test.h is the GoogleTest layer over it. The fixture
// library is built -fno-exceptions -fno-rtti and gtest's headers configure themselves from
// those flags, so including them on both sides of that boundary puts two different gtest
// configurations into one link.
//
// FOUR THINGS TO KNOW BEFORE WRITING AN ARM.
//
// 1. arch_switch returns instead of switching, so the state machine is committed and the
//    machine context is not. After any call that reschedules, kernel().current is the thread
//    the scheduler PICKED, and an arm that wants to keep speaking as its old thread must
//    re-seat it.
//
// 2. A blocking primitive's RETURN VALUE is worth exactly the waker an arm supplies. The
//    fixture poisons wait_result, credits the switch-in wq_confirm_resume spins for, and
//    calls the waker armed by wake_next_park, so an assertion on a result nobody wrote
//    cannot pass; a park with no waker armed ends the arm.
//
// 3. The switch REQUEST is the observable: every arch_switch lands in the trace, which is
//    how an arm gates a path whose real switch never returns, exit_current past its
//    on_remove being exactly that.
//
// 4. run_exit() ends the arm inside exit_current's arch_idle_wait park with a longjmp, which
//    requires the IrqLock scope to have closed. Never longjmp out of a held IrqLock, and
//    never out of a capability sweep: a run_in_chunk_gap action runs INSIDE a sweep, so
//    run_exit is not available to it.
//
//    reset() also restores the state cap.cc keeps outside the Kernel struct: cap_slab_init()
//    for the chunk free list and cap_console_reset() for g_stdout_target. Without that clear
//    a stale global handle survives into an arm whose gen-encoded handles therefore REPEAT,
//    and an unrelated endpoint close matches it and notes a console death in ANOTHER arm's
//    counter. An arm that abandons a sweep would leave cap_teardown_active() true for every
//    later arm, which reads as a suite that passes: reset() REFUSES rather than continuing.

#ifndef KICKOS_TESTS_UNIT_KFIXTURE_KFIXTURE_H
#define KICKOS_TESTS_UNIT_KFIXTURE_KFIXTURE_H

#include <stdint.h>

#include <kickos/domain.h>
#include <kickos/endpoint.h>
#include <kickos/sched.h>
#include <kickos/task.h>
#include <kickos/thread.h>

namespace kickos
{
    namespace testfix
    {
        // Answered by the arch_in_isr stub. The thread-context/ISR-context invariant is
        // enforced by a kpanic, so a gate for it is a death test.
        extern bool g_in_isr;
        extern uint64_t g_now_ns;

        // The fake domain pool the seam's domain_for/ref/release work over. The creator hold
        // and the members' hold are TWO references on one domain.
        extern Domain g_domains[KICKOS_MAX_TASKS];
        extern uint16_t g_domain_refs[KICKOS_MAX_TASKS];
        extern bool g_domain_live[KICKOS_MAX_TASKS];
        // -1 for null and for anything outside the pool.
        int domain_index(Domain const* d);
        uint16_t domain_refs(Domain const* d);

        extern uint32_t g_switches;
        extern uint32_t g_console_noted;
        extern uint32_t g_console_reclaimed;
        extern uint32_t g_parked;

        // Every TCB an arm may drive without the ThreadPool, in plain storage.
        constexpr int MAX_TEST_THREADS = 8;

        struct Fixture
        {
            Thread idle;
            Thread t[MAX_TEST_THREADS];
        };

        extern Fixture g_fx;

        // --- the ordered trace ---------------------------------------------------------
        // Asserted as ONE string with EXPECT_STREQ: the claims are about ORDER, and a
        // counter oracle cannot fail on a reordering.
        char const* trace();
        __attribute__((format(printf, 1, 2))) void trace_add(char const* fmt, ...);
        void trace_reset();

        // --- the seam's recorders ------------------------------------------------------
        Thread* thread_of_context(struct arch_context* c);
        void note_switch(Thread* from, Thread* to);
        // The context rebuild, recorded rather than performed. Its position in the trace pins
        // the rebuild before the switch, which is the placement rule the pended backends need.
        void note_ctx_redirect(Thread* t, void (*entry)(void* arg), void* base, size_t size);
        // The last rebuild's arguments. Null / 0 until one happens; cleared by reset().
        extern Thread* g_redirect_target;
        extern void (*g_redirect_entry)(void* arg);
        extern uintptr_t g_redirect_stack_top;
        extern uint32_t g_redirects;
        void note_park();
        void note_irq_save();
        void note_irq_restore();

#if KICKOS_KERNEL_CORES > 1
        // --- the kernel lock, and the swap that carries it -----------------------------
        // Above one core the seam owns the lock word and the swap ends its span the way a
        // backend's assembly does, by calling kickos_switch_unlock once the outgoing frame is
        // parked.
        //
        // Which half of the swap the seam performs. IMMEDIATE parks inside arch_switch;
        // DEFERRED books it and parks at the next arch_idle_wait, as a backend whose swap runs
        // in an exception epilogue does. Set it before the call that reschedules; reset() puts
        // it back to IMMEDIATE.
        enum class SwapMode
        {
            IMMEDIATE,
            DEFERRED
        };
        void set_swap_mode(SwapMode m);

        // The core identity every keyed read in the compiled sources resolves through.
        extern uint32_t g_core;

        bool klock_held();
        void note_klock_acquire();
        void note_klock_release();

        // Swaps whose park committed, and how many of them found the lock free.
        extern uint32_t g_parks_committed;
        extern uint32_t g_parks_without_lock;
        // Releases taken while the running thread was EXITED and no swap had parked it: each
        // one is a window in which a peer reads that state off a thread whose saved frame
        // still describes an earlier run.
        extern uint32_t g_exit_window_opened;
        // The thread the last committed park saved, and the state it carried there.
        extern Thread* g_park_from;
        extern ThreadState g_park_from_state;

        extern uint32_t g_ipi_sends;
        extern uint32_t g_ipi_send_mask;
#endif

        // --- the waker a real park needs -----------------------------------------------
        // Called with the parked thread's wait_result already poisoned, as the thread the
        // scheduler picked: switch_to publishes `current` before arch_switch, so a waker
        // that speaks for the incoming thread (mutex_unlock, sem_post) is already seated.
        // MUST NOT itself block: resolve_park clears g_park_waker before calling it, so a
        // waker that parks re-enters with none armed, misattributing the failure.
        using ParkWaker = void (*)(Thread* parked);
        // Arms the ONE waker the next park may consume. See note 2.
        void wake_next_park(ParkWaker fn);

        // --- the chunk gap -------------------------------------------------------------
        // Runs `fn` at the `ordinal`th moment no IrqLock is held, counting from the arming
        // call, and traces every such moment as gap<n>. cap_teardown opens one before its
        // first chunk and one after each, so an ordinal names a chunk boundary. Gaps `fn`
        // itself opens are neither counted nor traced. One-shot; disarmed by reset().
        using GapAction = void (*)();
        void run_in_chunk_gap(GapAction fn, uint32_t ordinal);

        // --- driving -------------------------------------------------------------------
        // Fresh kernel, fresh scheduler, one idle thread, started.
        void reset();
        // A thread the scheduler knows about, READY at `prio`, in fixture storage.
        Thread* spawn(int slot, uint8_t prio);
        // A TCB the exit sweep can FIND: exit_current's join lookup IS the ThreadPool scan.
        Thread* seat_pool(int slot, uint8_t prio);
        // Seated into wait_result by every park below and by the park resolver of note 2, so
        // an arm that expects a waker to have written one cannot be satisfied by a zeroed TCB.
        constexpr intptr_t WAIT_RESULT_POISON = -424242;
        // WAIT_JOIN is parked on no list at all; the tag is the only edge back.
        void park_join(Thread* w, Thread* target);
        Endpoint* endpoint();
        // Give `t` a real capability table of `width` slots from the real slab, so an arm can
        // put the REAL cap_teardown through a live entry. Dies if the slab refuses.
        void attach_caps(Thread* t, uint32_t width);
        // The creator tag task() mints with: it names no pool slot, so no arm's exiting thread
        // orphans a hand-made group. Tag 1 would, being pool slot 0's tag, which is the slot
        // the arms seat their dying thread into.
        constexpr uint16_t FIXTURE_TASK_TAG = KICKOS_THREAD_SLOTS + 1;
        static_assert(FIXTURE_TASK_TAG < 0xFFu,
                      "the fixture's creator tag would alias idle's boot tag once truncated "
                      "into Task::creator_tag, and every arm's exiting thread would orphan "
                      "the groups task() minted");

        // An EXPLICIT task minted by the REAL task_create, so its creator hold, its domain
        // reference and its slot reservation are the shipping ones. Dies unless the mint
        // lands in `slot`: free_slot() scans upward, so call these in slot order.
        Task* task(int slot);
        // `t` joins `tk` through the real task_ref, which is what takes the members' domain
        // reference. Group membership is a pointer comparison over the thread pool, so only
        // the TCB field matters to the scan.
        void join_task(Thread* t, Task* tk);
        // A semaphore park: the kind with no error channel at all, so an arm can show the
        // cancel reaches it anyway and leaves the token count alone. out_handle may be null.
        Semaphore* semaphore(int* out_handle);
        void park_sem_waiter(Thread* w, Semaphore* s);
        // A sleeper on the timer delta list. On no wait queue, so the tag is the only edge.
        void park_sleeper(Thread* w, uint64_t deadline_ns);
        // A PLAIN sender (call_state CALL_NONE) parked on ep->send_waiters: a CALL_SEND_WAIT
        // caller boosts the server, so only this shape can carry a priority the dying server
        // does not already hold.
        void park_plain_sender(Thread* w, Endpoint* ep);
        // A mutex `owner` HOLDS, from the real pool, seated with TWO object refs because a
        // waiter necessarily holds a cap of its own: at one ref the sweep's drop reaches zero
        // with the ownership just transferred and trips mutex_ref_drop's owner assert.
        Mutex* own_mutex(Thread* owner, int* out_handle);
        void park_mutex_waiter(Thread* w, Mutex* m);
        // Runs the REAL sched::exit_current and returns once it parks.
        void run_exit(int code);
        // The same exit as a CONTAINED FAULT rather than a return: a fault carries no
        // cancel_kind, and the two scope the death differently (kernel/sched/sched.cc).
        void run_exit_faulted(int code);
        void run_exit_as(int code, sched::ExitCause cause);

        // DEATH-TEST FORKED CHILD ONLY. gtest matches the child's STDERR and karch_seam.cc's
        // kpanic writes stdout, which tests/lib/panic.ere gates on target. Folding the other
        // way (fd 1 onto fd 2) replaces gtest's own capture pipe, and every death case then
        // reports an empty child message.
        void fold_stdout_into_stderr();
    }
}

#endif
