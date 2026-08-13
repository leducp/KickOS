// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Host fixture for the kernel's own state machines: a real Kernel instance, the real
// scheduler, the real FIFO/RR policy, the real capability teardown, with karch_seam.cc
// standing in for the arch boundary and the four subsystems those sources call out to.
//
// This header is deliberately GTEST-FREE, and kseam_test.h is the GoogleTest layer over it.
// The fixture library is compiled -fno-exceptions -fno-rtti, gtest's headers configure
// themselves from those flags, and including them on both sides of that boundary would put two
// different gtest configurations into one link.
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
//    capability sweep.
//
//    A run_in_chunk_gap action runs INSIDE a sweep for the same reason, so it may not
//    longjmp either, and run_exit is therefore not available to it.
//
//    THE RESET IS NOT TOTAL, because cap.cc keeps THREE data outside the Kernel struct that
//    `kernel() = Kernel{}` reaches, all in one TU-local constinit:
//      - the chunk free list: reset() restores it with cap_slab_init().
//      - teardown_depth: unreachable from outside cap.cc. An arm that abandons a sweep would
//        leave cap_teardown_active() true for every later arm, which reads as a suite that
//        passes, so reset() REFUSES rather than continuing.
//      - g_stdout_target: also unreachable, and the trap has teeth. No arm may call
//        cap_console_publish today: it leaves a global endpoint handle set, reset() zeroes the
//        endpoint pool so gen-encoded handles REPEAT across arms, and a later arm's dying
//        thread closing an unrelated endpoint cap can then match the stale value and note a
//        console death. The symptom surfaces in a DIFFERENT arm's counter.

#ifndef KICKOS_KFIXTURE_H
#define KICKOS_KFIXTURE_H

#include <stdint.h>

#include <kickos/endpoint.h>
#include <kickos/task.h>
#include <kickos/thread.h>

namespace kickos
{
    namespace testfix
    {
        // Answered by the arch_in_isr stub. The invariant separating thread context from
        // ISR context is enforced by a kpanic, so a gate for it is a process that dies.
        extern bool g_in_isr;
        extern uint64_t g_now_ns;

        extern uint32_t g_switches;
        extern uint32_t g_console_noted;
        extern uint32_t g_console_reclaimed;
        extern uint32_t g_parked;

        // Every TCB an arm may drive without the ThreadPool. Plain storage, because nothing
        // gated here resolves a thread by handle and the pool's reclaim rules would make the
        // arms about the pool. The two exceptions have their own helpers: endpoint() and
        // seat_pool().
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
        // Two of the mutants this gate kills are pure reorderings.
        char const* trace();
        __attribute__((format(printf, 1, 2))) void trace_add(char const* fmt, ...);
        void trace_reset();

        // --- the seam's recorders ------------------------------------------------------
        Thread* thread_of_context(struct arch_context* c);
        void note_switch(Thread* from, Thread* to);
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
        // A task slot from the real pool, EXPLICIT (it carries a creator tag), so an arm can
        // put threads in one group. task_release is a seam stub here, so the refcount is
        // bookkeeping an arm may read and nothing frees the slot.
        Task* task(int slot);
        // `t` joins `tk`. Only the TCB field matters to the group scan: membership is a
        // pointer comparison over the thread pool, not a list the task holds.
        void join_task(Thread* t, Task* tk);
        // A semaphore park: the kind with NO error channel at all, so an arm can show that the
        // cancel reaches it anyway and that the token count is left alone.
        // out_handle may be null: an arm that never installs a cap on the semaphore does not
        // need one, and only the object identity matters to a park.
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
