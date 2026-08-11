// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Host fixture for the kernel's own state machines: a real Kernel instance, the real
// scheduler, the real FIFO/RR policy, the real capability teardown, with karch_seam.cc
// standing in for the arch boundary and the four subsystems those sources call out to.
//
// FOUR THINGS TO KNOW BEFORE WRITING AN ARM.
//
// 1. arch_switch does NOT switch. The whole state machine (run state, current, the ready
//    lists, every priority) is committed before switch_to reaches it, so a stub that
//    returns is exactly "run the machine, skip the machine context". The TEST is therefore
//    the CPU: after any call that reschedules, kernel().current is the thread the scheduler
//    PICKED, and an arm that wants to keep speaking as its old thread must re-seat it.
//
// 2. A blocking primitive's park STATE is assertable; its RETURN VALUE is a fiction,
//    because no waker ever wrote wait_result. mutex_lock ends in wq_confirm_resume, which
//    spins on the blocker's switch_count until a real switch-in bumps it, so with a
//    returning arch_switch it would spin to KICKOS_POLL_SPIN_MAX and panic;
//    g_resume_on_switch collapses "parked, switched away, switched back" into the stub by
//    crediting the OUTGOING thread. NO ARM DRIVES A BLOCKING PRIMITIVE YET, so this note is
//    a warning about the first one that does and the mechanism under it is unexercised.
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
#include <kickos/thread.h>

namespace kickos
{
    namespace testfix
    {
        // Answered by the arch_switch stub: credits the OUTGOING thread with the switch-in
        // that would have brought it back, which is what lets a blocking primitive's
        // resume barrier complete. See note 2.
        extern bool g_resume_on_switch;
        // Answered by the arch_in_isr stub. The invariant separating thread context from
        // ISR context is enforced by a kpanic, so a gate for it is a process that dies.
        extern bool g_in_isr;
        extern uint64_t g_now_ns;

        extern unsigned g_switches;
        extern unsigned g_console_noted;
        extern unsigned g_console_reclaimed;
        extern unsigned g_parked;
        extern int g_failures;

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
        // An oracle over the full ordered trace, never a set of counters: the claims worth
        // gating here are about ORDER, and a counter oracle cannot fail on a reordering.
        char const* trace();
        __attribute__((format(printf, 1, 2))) void trace_add(char const* fmt, ...);
        void trace_reset();

        // --- checks --------------------------------------------------------------------
        void check(bool ok, char const* what);
        void check_eq(unsigned got, unsigned want, char const* what);
        void check_trace(char const* want, char const* what);

        // --- the seam's recorders ------------------------------------------------------
        Thread* thread_of_context(struct arch_context* c);
        void note_switch(Thread* from, Thread* to);
        void note_park();

        // --- driving -------------------------------------------------------------------
        // Fresh kernel, fresh scheduler, one idle thread, started.
        void reset();
        // A thread the scheduler knows about, READY at `prio`, in fixture storage.
        Thread* spawn(int slot, uint8_t prio);
        // A TCB the exit sweep can FIND: exit_current's join lookup IS the ThreadPool scan.
        Thread* seat_pool(int slot, uint8_t prio);
        // Seated into a parked waiter's wait_result by park_join, so an arm that expects a
        // waker to have written one cannot be satisfied by a zeroed TCB.
        constexpr intptr_t WAIT_RESULT_POISON = -424242;
        // WAIT_JOIN is parked on no list at all; the tag is the only edge back.
        void park_join(Thread* w, Thread* target);
        Endpoint* endpoint();
        // Give `t` a real capability table of `width` slots from the real slab, so an arm can
        // put the REAL cap_teardown through a live entry. Dies if the slab refuses.
        void attach_caps(Thread* t, uint32_t width);
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
    }
}

#endif
