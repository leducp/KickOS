// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Host fixture using the real kernel state, scheduler, capability teardown
// and task pool, with architecture hooks from karch_seam.cc.
// Keep GTest out of this header: fixture and test code use different
// exception/RTTI flags. Use kseam_test.h for GTest helpers.
//
// arch_switch records a request and returns without changing machine context.
// After rescheduling, Kernel::current names the selected thread; tests must
// restore the caller explicitly if needed. Blocking tests must install a
// nonblocking waker with wake_next_park. wait_result is poisoned until written.
//
// run_exit returns by longjmp from the final idle park. Never use it while
// holding IrqLock or inside a capability-sweep gap. reset clears global
// capability state and rejects an unfinished sweep.

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
        // Value returned by arch_in_isr. Invalid call context is checked by death tests.
        extern bool g_in_isr;
        extern uint64_t g_now_ns;

        // Separate creator and member references. Reserve one more domain than task
        // slots so task-pool exhaustion can be tested after successful admission.
        constexpr int FIXTURE_DOMAIN_SLOTS = KICKOS_MAX_TASKS + 1;
        extern Domain g_domains[FIXTURE_DOMAIN_SLOTS];
        extern uint16_t g_domain_refs[FIXTURE_DOMAIN_SLOTS];
        extern bool g_domain_live[FIXTURE_DOMAIN_SLOTS];
        // -1 for null and for anything outside the pool.
        int domain_index(Domain const* d);
        uint16_t domain_refs(Domain const* d);

        extern uint32_t g_switches;
        extern uint32_t g_console_noted;
        extern uint32_t g_console_reclaimed;
        extern uint32_t g_parked;

        // Fixture-owned TCB storage outside ThreadPool.
        constexpr int MAX_TEST_THREADS = 8;

        struct Fixture
        {
            Thread idle;
            Thread t[MAX_TEST_THREADS];
        };

        extern Fixture g_fx;

        // Compare ordered traces as strings to detect reordered operations.
        char const* trace();
        __attribute__((format(printf, 1, 2))) void trace_add(char const* fmt, ...);
        void trace_reset();

        // --- the seam's recorders ------------------------------------------------------
        Thread* thread_of_context(struct arch_context* c);
        void note_switch(Thread* from, Thread* to);
        // Record context rebuild order without changing machine state.
        void note_ctx_redirect(Thread* t, void (*entry)(void* arg), void* base, size_t size);
        // The last rebuild's arguments. Null / 0 until one happens; cleared by reset().
        extern Thread* g_redirect_target;
        extern void (*g_redirect_entry)(void* arg);
        extern uintptr_t g_redirect_stack_top;
        extern uint32_t g_redirects;
        void note_park();
#if KICKOS_HAVE_ASPACE
        // Domain-release hook for translating fixtures; used by deathspace.
        void note_member_release();
#endif
        void note_irq_save();
        void note_irq_restore();
        // Brackets currently open, counted by the seam's save and restore.
        uint32_t irq_depth();

        // Whether each line's last mask or unmask was an unmask, and that answer for the line
        // arch_irq_route was last handed, taken when it was handed it: -1 for never called.
        extern bool g_line_armed[KICKOS_MAX_IRQ];
        extern int g_routed_armed;
        // arch_irq_clear_pending calls per line.
        extern uint32_t g_line_clears[KICKOS_MAX_IRQ];

#if KICKOS_KERNEL_CORES > 1
        // Simulate switch completion and release the kernel lock after parking.
        // IMMEDIATE parks in arch_switch; DEFERRED waits until arch_idle_wait.
        // Set before rescheduling; reset restores IMMEDIATE.
        enum class SwapMode
        {
            IMMEDIATE,
            DEFERRED
        };
        void set_swap_mode(SwapMode m);

        extern uint32_t g_core;

        bool klock_held();
        void note_klock_acquire();
        void note_klock_release();

        // Committed parks and parks that found the lock free.
        extern uint32_t g_parks_committed;
        extern uint32_t g_parks_without_lock;
        // Count lock releases that expose EXITED before the outgoing frame is saved.
        extern uint32_t g_exit_window_opened;
        // The thread the last committed park saved, and the state it carried there.
        extern Thread* g_park_from;
        extern ThreadState g_park_from_state;

        // Cross-core raises and the peers they named, and raises a core made on itself.
        extern uint32_t g_ipi_sends;
        extern uint32_t g_ipi_send_mask;
        extern uint32_t g_ipi_self_raises;
#endif

        // The waker runs as the selected thread with the parked wait_result poisoned.
        // It must not block: the callback is cleared before invocation.
        using ParkWaker = void (*)(Thread* parked);
        // Set the one-shot waker for the next park.
        void wake_next_park(ParkWaker fn);

        // Run fn at the specified lock-free gap and trace gaps as gap<n>.
        // Capability teardown opens gaps before and after chunks. Ignore nested gaps
        // inside fn. One-shot; reset disables it.
        using GapAction = void (*)();
        void run_in_chunk_gap(GapAction fn, uint32_t ordinal);
        // Gaps traced since the last run_in_chunk_gap.
        uint32_t gaps_seen();

        // Reset the kernel and start the scheduler with one idle thread.
        void reset();
        // Takes a READY thread off its ready list, or a HANDED one out of every ring entry naming
        // it, by hand. A fixture shortcut no kernel path takes: the kernel parks only a running
        // thread.
        void detach_ready(Thread* t);
#if KICKOS_KERNEL_CORES > 1
        // Rewrites every ring without the HANDOFF entries naming `t`, published ones staying
        // published. Returns whether any did.
        bool unstage(Thread const* t);
#endif
        // A thread the scheduler knows about, READY at `prio`, in fixture storage.
        Thread* spawn(int slot, uint8_t prio);
        // Use ThreadPool storage so exit_current can find the TCB.
        Thread* seat_pool(int slot, uint8_t prio);
        // Poison value for wait_result until the waker supplies a result.
        constexpr intptr_t WAIT_RESULT_POISON = -424242;
        // WAIT_JOIN is parked on no list at all; the tag is the only edge back.
        void park_join(Thread* w, Thread* target);
        Endpoint* endpoint();
        // Allocate a real capability table; fail if the slab cannot supply it.
        void attach_caps(Thread* t, uint32_t width);
        // Use a creator tag outside ThreadPool so test-thread exit cannot orphan tasks.
        constexpr uint16_t FIXTURE_TASK_TAG = KICKOS_THREAD_SLOTS + 1;
        static_assert(FIXTURE_TASK_TAG < 0xFFu,
                      "the fixture's creator tag would alias idle's boot tag once truncated "
                      "into Task::creator_tag, and every arm's exiting thread would orphan "
                      "the groups task() minted");

        // Create an explicit task with real holds and references. Call in slot order;
        // fail if task_create returns a different slot.
        Task* task(int slot);
        // Join through task_ref and store the task pointer used by membership scans.
        void join_task(Thread* t, Task* tk);
        // Park on a semaphore. out_handle may be null.
        Semaphore* semaphore(int* out_handle);
        void park_sem_waiter(Thread* w, Semaphore* s);
        // A sleeper on the timer delta list. On no wait queue, so the tag is the only edge.
        void park_sleeper(Thread* w, uint64_t deadline_ns);
        // Park a plain sender without CALL_SEND_WAIT priority donation.
        void park_plain_sender(Thread* w, Endpoint* ep);
        // Create a held mutex with two object references, including the waiter's.
        // One reference would free it immediately after ownership transfer.
        Mutex* own_mutex(Thread* owner, int* out_handle);
        void park_mutex_waiter(Thread* w, Mutex* m);
        // Run sched::exit_current until its final park.
        void run_exit(int code);
        // Exit as a contained fault, without a cancellation kind.
        void run_exit_faulted(int code);
        void run_exit_as(int code, sched::ExitCause cause);
        // Run fn, which must reach sched::exit_current, until the final park. The frames the
        // longjmp skips hold brackets the death path has already ended.
        void run_noreturn(void (*fn)());

        // Use only in a GTest death-test child. Redirect panic output to the existing
        // stderr capture pipe without replacing that pipe.
        void fold_stdout_into_stderr();
    }
}

#endif
