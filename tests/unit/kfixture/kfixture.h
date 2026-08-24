// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Host fixture for the kernel's own state machines: a real Kernel instance, the real
// scheduler, the real FIFO/RR policy, the real capability teardown, the real task pool, with
// karch_seam.cc standing in for the arch boundary and the subsystems those sources call out to.
//
// Keep this header GTEST-FREE; kseam_test.h is the GoogleTest layer over it. The fixture
// library is built -fno-exceptions -fno-rtti and gtest's headers configure themselves from
// those flags, so including them on both sides of that boundary puts two different gtest
// configurations into one link.
//
// FOUR THINGS TO KNOW BEFORE WRITING AN ARM.
//
// 1. arch_switch does NOT switch. The whole state machine (run state, current, the ready
//    lists, every priority) is committed before switch_to reaches it, so a stub that
//    returns is exactly "run the machine, skip the machine context". The TEST is therefore
//    the CPU: after any call that reschedules, kernel().current is the thread the scheduler
//    PICKED, and an arm that wants to keep speaking as its old thread must re-seat it.
//
// 2. A blocking primitive's RETURN VALUE is worth exactly the waker an arm supplies. The
//    thread that parks gets the CPU straight back, so the fixture stands in for the other
//    side of the switch: it POISONS wait_result, credits the switch-in wq_confirm_resume
//    spins for, and calls the waker armed by wake_next_park. A park with none armed ends
//    the arm, and a waker that writes no result leaves the poison, so an assertion on a
//    result nobody wrote cannot pass.
//
// 3. A switch REQUEST is the observable, not its effect. Every arch_switch lands in the
//    trace, so an arm asserts which switch happened and WHEN relative to the other calls.
//    That is the only way to gate a path whose real switch never returns, exit_current
//    past its on_remove being exactly that.
//
// 4. exit_current does not return on target: it parks in arch_idle_wait forever. run_exit()
//    ends the arm there with a longjmp, which is safe only because the IrqLock scope closes
//    before that park loop. Never longjmp out of a held IrqLock, and never out of a
//    capability sweep. A run_in_chunk_gap action runs INSIDE a sweep, so it may not longjmp
//    either and run_exit is not available to it.
//
//    reset() also restores the state cap.cc keeps outside the Kernel struct, which
//    re-constructing the Kernel does not reach: cap_slab_init() for the chunk free list, and
//    cap_console_reset() for g_stdout_target, which is what lets an arm publish a console at
//    all. Without that clear, a stale global handle survives into an arm whose endpoint pool
//    has been zeroed and whose gen-encoded handles therefore REPEAT, and an unrelated
//    endpoint close matches it and notes a console death in ANOTHER arm's counter. cap.cc's
//    teardown_depth is unreachable from outside it, so an arm that abandons a sweep would
//    leave cap_teardown_active() true for every later arm, which reads as a suite that
//    passes: reset() REFUSES rather than continuing.

#ifndef KICKOS_TESTS_UNIT_KFIXTURE_KFIXTURE_H
#define KICKOS_TESTS_UNIT_KFIXTURE_KFIXTURE_H

#include <stdint.h>

#include <kickos/domain.h>
#include <kickos/endpoint.h>
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
        // and the members' hold are TWO references on one domain, and telling them apart is
        // what gates which of them a death drops.
        extern Domain g_domains[KICKOS_MAX_TASKS];
        extern uint16_t g_domain_refs[KICKOS_MAX_TASKS];
        extern bool g_domain_live[KICKOS_MAX_TASKS];
        // -1 for null and for anything outside the pool, so a release of a task with no
        // domain is inert exactly as the real one is.
        int domain_index(Domain const* d);
        uint16_t domain_refs(Domain const* d);

        extern uint32_t g_switches;
        extern uint32_t g_console_noted;
        extern uint32_t g_console_reclaimed;
        extern uint32_t g_parked;

        // Every TCB an arm may drive without the ThreadPool. Plain storage, because nothing
        // gated here resolves a thread by handle and the pool's reclaim rules would make the
        // arms about the pool.
        constexpr int MAX_TEST_THREADS = 8;

        struct Fixture
        {
            Thread idle;
            Thread t[MAX_TEST_THREADS];
        };

        extern Fixture g_fx;

        // --- the ordered trace ---------------------------------------------------------
        // Asserted as ONE string with EXPECT_STREQ, never as a set of counters: the claims
        // worth gating here are about ORDER, and a counter oracle cannot fail on a reordering.
        char const* trace();
        __attribute__((format(printf, 1, 2))) void trace_add(char const* fmt, ...);
        void trace_reset();

        // --- the seam's recorders ------------------------------------------------------
        Thread* thread_of_context(struct arch_context* c);
        void note_switch(Thread* from, Thread* to);
        // The context rebuild, recorded rather than performed: an arm sees WHICH thread's
        // context was named and WITH WHAT, and its position in the trace pins the rebuild
        // before the switch, which is the placement rule the pended backends need.
        void note_ctx_redirect(Thread* t, void (*entry)(void* arg), void* base, size_t size);
        // The last rebuild's arguments, for an arm that asserts the entry stub and the stack
        // top rather than only the ordering. Null / 0 until one happens; cleared by reset().
        extern Thread* g_redirect_target;
        extern void (*g_redirect_entry)(void* arg);
        extern uintptr_t g_redirect_stack_top;
        extern uint32_t g_redirects;
        void note_park();
        void note_irq_save();
        void note_irq_restore();

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
        // first chunk and one after each, so an ordinal names a chunk boundary and `fn`
        // sees a sweep that holds nothing. Gaps `fn` itself opens are neither counted nor
        // traced. One-shot; disarmed by reset().
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
        // The creator tag task() mints with. It names NO pool slot and is not the boot tag
        // kill_tag_of answers for a thread outside the pool, so no arm's exiting thread
        // orphans a hand-made group by accident. Tag 1 would: it is pool slot 0's tag, and
        // that is the slot the arms seat their dying thread into.
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
        // reference. Only the TCB field matters to the group scan: membership is a pointer
        // comparison over the thread pool, not a list the task holds.
        void join_task(Thread* t, Task* tk);
        // A semaphore park: the kind with NO error channel at all, so an arm can show that the
        // cancel reaches it anyway and that the token count is left alone. out_handle may be
        // null; only the object identity matters to a park.
        Semaphore* semaphore(int* out_handle);
        void park_sem_waiter(Thread* w, Semaphore* s);
        // A sleeper on the timer delta list. On no wait queue, so the tag is the only edge.
        void park_sleeper(Thread* w, uint64_t deadline_ns);
        // A PLAIN sender (call_state CALL_NONE) parked on ep->send_waiters. Plain is the
        // point: a CALL_SEND_WAIT caller boosts the server, so only this shape can carry a
        // priority the dying server does not already hold.
        void park_plain_sender(Thread* w, Endpoint* ep);
        // A mutex `owner` HOLDS, from the real pool, seated with TWO object refs because a
        // waiter necessarily holds a cap of its own: at one ref the sweep's drop reaches zero
        // with the ownership just transferred and trips mutex_ref_drop's R4 assert.
        Mutex* own_mutex(Thread* owner, int* out_handle);
        void park_mutex_waiter(Thread* w, Mutex* m);
        // Runs the REAL sched::exit_current and returns once it parks.
        void run_exit(int code);

        // Called INSIDE a death test's forked child, by KICKOS_EXPECT_PANIC only. gtest matches
        // the child's STDERR and karch_seam.cc's kpanic writes stdout, which it must keep doing:
        // that stream is what tests/lib/panic.ere gates on target. Folding the other way (fd 1
        // onto fd 2) replaces gtest's own capture pipe, and every death case then reports an
        // empty child message.
        void fold_stdout_into_stderr();
    }
}

#endif
