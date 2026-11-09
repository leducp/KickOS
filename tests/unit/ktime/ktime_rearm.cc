// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Host gate on the two properties every arch one-shot backend depends on.
//
// Whether ktime_rearm reprograms the comparator must be a function of what is pending, and of
// nothing else; in particular, not of when the call happens. No backend dedups an arm:
// arch_timer_arm reprograms whatever it is handed, and the kernel is the sole authority for
// what each comparator holds. A rearm whose dedup key moves with the clock defeats that:
// ktime_rearm runs on EVERY context switch, so the countdown restarts before it can reach the
// compare, and the sleeper is held off for as long as the switches keep coming.
//
// What it hands arch_timer_arm, once it does program, is never closer than the min delta to the
// clock it reads then.
//
// The other half is that a comparator which has FIRED no longer holds what the kernel
// recorded. A backend that clamps a far deadline into a narrow counter fires early and is
// re-armed for the same absolute deadline, so a skip there starves the sleeper for good.
//
// The armed deadline is arch-internal, and this gate links the REAL kernel/time/time.cc
// against a fake clock and a recording timer, so it reads the exact value handed to
// arch_timer_arm.

#include <kickos/irqlock.h>
#include <kickos/time.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/instance.h>
#include <kickos/arch/arch.h>

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

#include <gtest/gtest.h>

namespace
{
    uint64_t g_now = 0;            // fake monotonic clock, advanced only by the test
    uint64_t g_armed = UINT64_MAX; // last deadline handed to arch_timer_arm
    uint32_t g_arms = 0;
    uint32_t g_disarms = 0;
    uint64_t g_slice = UINT64_MAX; // the policy deadline next_timed_event answers for a thread
    uint32_t g_switch_ins = 0;
    uint32_t g_irq_saves = 0;
}

// --- the seam under the unit: a fake clock and a recording timer ---
extern "C"
{
    arch_irq_state_t arch_irq_save(void)
    {
        g_irq_saves++;
        return 0;
    }
    void arch_irq_restore(arch_irq_state_t) {}

    uint64_t arch_clock_now(void) { return g_now; }

    void arch_timer_arm(uint64_t deadline_ns)
    {
        g_armed = deadline_ns;
        g_arms++;
    }

    void arch_timer_disarm(void)
    {
        g_armed = UINT64_MAX;
        g_disarms++;
    }

    // time.cc includes ktrace.h, whose inline emitters call both under the presets that
    // carry telemetry; without these stubs the gate fails at LINK on sim-telem.
    uint32_t arch_trace_now(void) { return 0; }
    void kickos_rtt_write_record_ch1(void const*, unsigned long) {}
}

namespace kickos
{
    namespace detail
    {
        InstanceLocal<Kernel> g_instance;
#if KICKOS_MULTI_INSTANCE
        // instance.cc is not linked here, so the selector's storage is owed too. One
        // instance, so the index stays 0.
        __thread unsigned g_instance_index __attribute__((tls_model("initial-exec"))) = 0;
#endif
    }

    // time.cc calls kpanic when ktime_on_timer finds a deadline under a park that cannot
    // time out. kpanic is noreturn: a stub that returns is undefined behaviour, and would
    // present as this gate passing through a kernel invariant violation.
    void kpanic(char const* msg)
    {
        fprintf(stderr, "kpanic: %s\n", msg);
        abort();
    }

    // ktime_on_timer delegates every park's unwind to park.cc, which this gate does not link.
    // Reaching this means an arm let a deadline expire, which no arm here stages, so abort for
    // the same reason as kpanic above.
    void thread_abort_park(Thread*, intptr_t, Held)
    {
        fprintf(stderr, "thread_abort_park: no park layer in this gate\n");
        abort();
    }


    // No death wait here either: the expiry loop's batch has nothing to leave.
    void death_leave_batch_begin()
    {
    }

    void death_leave_batch_end(Held)
    {
    }

    // realtime.cc is not linked here, and no arm here parks toward the wall clock.
    uint64_t realtime_now()
    {
        fprintf(stderr, "realtime_now: no wall clock in this gate\n");
        abort();
    }

    void realtime_deadline_arm(Thread*, uint64_t, Held)
    {
        fprintf(stderr, "realtime_deadline_arm: no wall clock in this gate\n");
        abort();
    }

    uint32_t park_queueless(Thread* c, WaitKind kind, void* obj, Held)
    {
        c->state.block();
        c->wait_queue = nullptr;
        c->wait_kind = kind;
        c->wait_obj = obj;
        return c->switch_count;
    }

    void policy_on_switch_in(Thread*)
    {
        g_switch_ins++;
    }

    namespace sched
    {
        // exit_current is noreturn, so the arm resumes through this rather than returning.
        // Set by an arm that expects the death point; reaching it with no landing pad staged
        // is a park that exited where no arm asked it to.
        bool g_exited = false;
        jmp_buf g_exit_pad;
        bool g_exit_pad_armed = false;

        void exit_current(int, ExitCause, IrqLock* held)
        {
            if (held != nullptr)
            {
                held->end();
            }
            g_exited = true;
            if (not g_exit_pad_armed)
            {
                fprintf(stderr, "exit_current: no landing pad staged for this arm\n");
                abort();
            }
            longjmp(g_exit_pad, 1);
        }

        // The seam reports "no timed event" and the reschedule only arms the timer as the
        // switch would, which leaves the sleeper on the sleepq for an arm to read.
        uint64_t next_timed_event(Thread const* t)
        {
            if (t == nullptr)
            {
                return UINT64_MAX;
            }
            return g_slice;
        }
        Thread* current()
        {
            return kernel().current(kickos_kernel_core());
        }
        void reschedule(Thread*, Held held)
        {
            ktime_rearm(current(), held);
        }
        void wake(Thread*, Held) {}
        void tick_rr(uint64_t, Held) {}
        void yield() {}
    }
}

namespace
{
    using namespace kickos;

    Thread g_sleeper;

    void reset()
    {
        kernel().sleepq = nullptr;
        kernel().current(kickos_kernel_core()) = &g_sleeper;
        g_sleeper.tnext = nullptr;
        g_sleeper.on_timer = false;
        g_sleeper.cancel_kind = CANCEL_NONE;
        g_sleeper.dying = false;
        g_sleeper.state.to<ThreadState::RUNNING>(); // the caller of a sleep syscall
        sched::g_exited = false;
        sched::g_exit_pad_armed = false;
        g_armed = UINT64_MAX;
        g_arms = 0;
        g_disarms = 0;
        g_slice = UINT64_MAX;
        g_switch_ins = 0;
        kernel().switch_landing_owed[kickos_kernel_core()] = 0;
        // What boot leaves: a value no computation can produce, so the first rearm of an arm
        // always programs.
        kernel().timer_armed_ns[kickos_kernel_core()] = 0;
    }

    void park(uint64_t deadline_ns)
    {
        g_sleeper.deadline_ns = deadline_ns;
        g_sleeper.on_timer = true;
        g_sleeper.tnext = nullptr;
        kernel().sleepq = &g_sleeper;
    }
}

// A deadline inside the min-delta window is the one case where a rearm is tempted to
// substitute now+min-delta for it as its dedup key. The comparator fires at the floor, and the
// switches behind it must leave it counting.
TEST(KTime, deadline_is_stable_inside_the_window)
{
    reset();
    uint64_t const deadline = 1000000; // 1 ms
    park(deadline);

    g_now = deadline - KICKOS_TIMER_MIN_DELTA_NS / 4; // well inside the window
    uint64_t const armed_at = g_now;
    {
        IrqLock lock;
        ktime_rearm(sched::current(), lock);
    }
    ASSERT_EQ(g_arms, 1u) << "the first rearm did not program the comparator";
    EXPECT_EQ(g_armed, armed_at + KICKOS_TIMER_MIN_DELTA_NS)
        << "a deadline inside the window was programmed closer than the min delta";
    EXPECT_EQ(kernel().timer_armed_ns[kickos_kernel_core()], deadline)
        << "the dedup key is not the pending deadline";

    // What a context switch does: same sleepq, later clock.
    for (uint32_t i = 0; i < 8; i++)
    {
        g_now += KICKOS_TIMER_MIN_DELTA_NS / 8;
        {
            IrqLock lock;
            ktime_rearm(sched::current(), lock);
        }
    }
    EXPECT_EQ(g_arms, 1u) << "a switch inside the window reprogrammed the comparator, which "
                             "restarts its countdown";
    EXPECT_EQ(g_armed, armed_at + KICKOS_TIMER_MIN_DELTA_NS);
}

// A deadline already due when the comparator is programmed: the work between its birth and the
// rearm outlasted the min delta, which is what a slow timer handler that books a switch does.
// Programmed in the past, the one-shot raises again before the handler returns, and on a core
// where the timer outranks the deferred switch no thread ever runs again. The deadline stays
// the dedup key, so the switches behind it do not move the comparator either.
TEST(KTime, a_due_deadline_fires_no_sooner_than_the_min_delta)
{
    reset();
    g_now = 5000000;
    uint64_t const deadline = g_now + KICKOS_TIMER_MIN_DELTA_NS; // born floored
    park(deadline);
    g_now += 2 * KICKOS_TIMER_MIN_DELTA_NS; // the handler's work after the birth
    uint64_t const armed_at = g_now;
    {
        IrqLock lock;
        ktime_rearm(sched::current(), lock);
    }
    ASSERT_EQ(g_arms, 1u);
    EXPECT_GE(g_armed, armed_at + KICKOS_TIMER_MIN_DELTA_NS)
        << "the comparator was programmed " << (armed_at - g_armed)
        << " ns in the past, so it raises again before the handler that armed it returns";
    EXPECT_EQ(kernel().timer_armed_ns[kickos_kernel_core()], deadline);

    g_now += 5 * KICKOS_TIMER_MIN_DELTA_NS;
    {
        IrqLock lock;
        ktime_rearm(sched::current(), lock);
    }
    EXPECT_EQ(g_arms, 1u) << "a due deadline was reprogrammed as the clock moved";
}

// The min-delta floor is not dropped, only moved: it belongs where the deadline is
// born, so that it is applied ONCE against ONE clock reading.
TEST(KTime, floor_is_applied_at_birth)
{
    reset();
    g_now = 5000000;
    ktime_sleep_until(g_now); // a deadline already due at the moment of the call
    EXPECT_EQ(kernel().sleepq, &g_sleeper) << "sleeper not parked";
    EXPECT_EQ(g_sleeper.deadline_ns, g_now + KICKOS_TIMER_MIN_DELTA_NS)
        << "an already-due sleep was not floored at birth";

    reset();
    g_now = 5000000;
    ktime_sleep_ns(1); // 1 ns: below the floor, and not the sleep(0) yield case
    EXPECT_EQ(kernel().sleepq, &g_sleeper) << "sub-floor sleeper not parked";
    EXPECT_EQ(g_sleeper.deadline_ns, g_now + KICKOS_TIMER_MIN_DELTA_NS)
        << "a sub-min-delta sleep did not round up to the min slice";

    // And a deadline born above the floor is passed through untouched.
    reset();
    g_now = 5000000;
    ktime_sleep_ns(1000000);
    EXPECT_EQ(g_sleeper.deadline_ns, g_now + 1000000) << "a normal sleep was perturbed";
}

// THE DEATH POINT ON THE SLEEP PATH. A sleep reaches no park funnel, so it carries its own
// prologue. The claim is that a cancelled caller ENDS here, and ends before the sleep has
// touched anything: no deadline written, no sleep-queue entry, no timer armed.
TEST(KTime, a_cancelled_sleeper_dies_before_it_touches_anything)
{
    reset();
    g_now = 5000000;
    g_sleeper.cancel_kind = CANCEL_SLAY;
    if (setjmp(sched::g_exit_pad) == 0)
    {
        sched::g_exit_pad_armed = true;
        ktime_sleep_until(g_now + 1000000);
        ADD_FAILURE() << "a cancelled caller parked instead of reaching its death point, and a "
                         "cancel that arrived behind the syscall entry read breaks no park";
    }
    sched::g_exit_pad_armed = false;
    EXPECT_TRUE(sched::g_exited) << "the sleep did not reach the death point at all";
    EXPECT_EQ(kernel().sleepq, nullptr)
        << "the dying thread was left on the sleep queue, which sched::exit_current does not "
           "sweep, so the list keeps a pointer into a slot the pool will re-hand";
    EXPECT_EQ(g_arms, 0u) << "a one-shot was armed for a thread that is not going to wake";
    EXPECT_FALSE(g_sleeper.on_timer) << "the dying thread kept its timer membership";
    EXPECT_NE(g_sleeper.state, ThreadState::BLOCKED) << "the caller was marked BLOCKED anyway";
}

// THE UNBOUNDED HALF. A sleeper normally self-wakes at its own deadline, so a cancel missed
// here costs the remaining delay and nothing more. ktime_sleep_ns saturates on overflow, and
// a saturated deadline is a park nothing ever ends.
TEST(KTime, a_cancelled_saturated_sleep_does_not_park_forever)
{
    reset();
    g_now = 5000000;
    g_sleeper.cancel_kind = CANCEL_KILL;
    if (setjmp(sched::g_exit_pad) == 0)
    {
        sched::g_exit_pad_armed = true;
        ktime_sleep_ns(UINT64_MAX); // saturates to a deadline no clock reaches
        ADD_FAILURE() << "a cancelled caller parked on a saturated deadline, which is a thread "
                         "lost for the lifetime of the image";
    }
    sched::g_exit_pad_armed = false;
    EXPECT_TRUE(sched::g_exited);
    EXPECT_EQ(kernel().sleepq, nullptr);
}

// The paired control, without which both arms above pass on a sleep that stopped working.
TEST(KTime, an_uncancelled_sleeper_still_parks)
{
    reset();
    g_now = 5000000;
    ktime_sleep_until(g_now + 1000000);
    EXPECT_FALSE(sched::g_exited) << "an uncancelled sleeper was ended at the death point";
    EXPECT_EQ(kernel().sleepq, &g_sleeper) << "an uncancelled sleeper did not park";
    EXPECT_EQ(g_arms, 1u) << "an uncancelled sleeper armed no one-shot";
}

// Tickless: nothing pending must disarm, not arm something far away.
TEST(KTime, empty_disarms)
{
    reset();
    g_now = 1234;
    {
        IrqLock lock;
        ktime_rearm(sched::current(), lock);
    }
    EXPECT_EQ(g_disarms, 1u) << "an empty sleepq did not disarm";
    EXPECT_EQ(g_arms, 0u) << "an empty sleepq armed instead of disarming";
}

// The dedup itself: what a switch burst costs the comparator once nothing pending has moved.
TEST(KTime, a_repeated_rearm_programs_the_comparator_once)
{
    reset();
    uint64_t const deadline = 1000000;
    park(deadline);
    {
        IrqLock lock;
        ktime_rearm(sched::current(), lock);
    }
    ASSERT_EQ(g_arms, 1u) << "the first rearm did not program the comparator";

    for (uint32_t i = 0; i < 8; i++)
    {
        g_now += KICKOS_TIMER_MIN_DELTA_NS;
        {
            IrqLock lock;
            ktime_rearm(sched::current(), lock);
        }
    }
    EXPECT_EQ(g_arms, 1u) << "a switch burst reprogrammed a comparator already holding the "
                             "only deadline anything pending has";
    EXPECT_EQ(g_disarms, 0u) << "a rearm with a sleeper still parked disarmed the comparator";
}

// The other half, and the whole correctness of the dedup: a comparator that has FIRED holds
// nothing, so the same deadline must be programmed again rather than recognised as a repeat.
// This is what a clamped backend does on every wrap of a deadline wider than its counter.
TEST(KTime, a_fired_comparator_is_reprogrammed_for_the_same_deadline)
{
    reset();
    uint64_t const deadline = 1000000;
    park(deadline);
    {
        IrqLock lock;
        ktime_rearm(sched::current(), lock);
    }
    ASSERT_EQ(g_arms, 1u) << "the first rearm did not program the comparator";

    // The counter ran out before the deadline: the one-shot fires early and the handler finds
    // nothing due, so what it re-arms is the same absolute deadline for its remainder.
    g_now = deadline / 2;
    ktime_on_timer();

    EXPECT_EQ(kernel().sleepq, &g_sleeper) << "an early fire woke a sleeper that is not due";
    EXPECT_EQ(g_armed, deadline) << "the remainder was armed for something other than the "
                                    "deadline that is still pending";
    EXPECT_EQ(g_arms, 2u) << "a fired comparator was left holding nothing: the rearm behind "
                             "the fire was skipped as a repeat of a deadline no longer "
                             "programmed, and the sleeper never wakes";
}

#if KICKOS_ARCH_SWITCH_LANDS_LATER
// A switch booked for a later exception: until it lands, the comparator holds no slice, so no
// expiry can be taken against a thread that has not run. The landing stamps the slice and
// programs it from the clock read there, however long the landing took.
TEST(KTime, a_booked_switch_arms_no_slice_until_it_lands)
{
    reset();
    g_now = 5000000;
    g_slice = g_now + 2 * KICKOS_TIMER_MIN_DELTA_NS; // the incoming thread's live deadline
    kernel().switch_landing_owed[kickos_kernel_core()] = 1;
    {
        IrqLock lock;
        ktime_rearm(sched::current(), lock);
    }
    EXPECT_EQ(g_arms, 0u) << "the slice of a thread not yet landed was armed";
    EXPECT_EQ(g_armed, UINT64_MAX);

    g_now += 3 * KICKOS_TIMER_MIN_DELTA_NS; // a slow switcher exception
    {
        IrqLock lock;
        ktime_land(lock);
    }
    EXPECT_EQ(g_switch_ins, 1u) << "the landing did not stamp the landed thread's slice";
    EXPECT_EQ(kernel().switch_landing_owed[kickos_kernel_core()], 0u);
    EXPECT_EQ(g_armed, g_now + KICKOS_TIMER_MIN_DELTA_NS)
        << "the landed thread's comparator was not programmed from the landing";
}

// A slice landing after the deadline the comparator already holds leaves the comparator alone.
TEST(KTime, a_landed_slice_after_the_armed_deadline_writes_no_comparator)
{
    reset();
    g_now = 5000000;
    park(g_now + 2 * KICKOS_TIMER_MIN_DELTA_NS);
    kernel().switch_landing_owed[kickos_kernel_core()] = 1;
    {
        IrqLock lock;
        ktime_rearm(nullptr, lock);
    }
    uint32_t const arms = g_arms;
    uint64_t const armed = g_armed;
    g_slice = g_now + 10 * KICKOS_TIMER_MIN_DELTA_NS;
    {
        IrqLock lock;
        ktime_land(lock);
    }
    EXPECT_EQ(g_switch_ins, 1u);
    EXPECT_EQ(kernel().switch_landing_owed[kickos_kernel_core()], 0u);
    EXPECT_EQ(g_arms, arms) << "the landing reprogrammed a comparator already due sooner";
    EXPECT_EQ(g_armed, armed);
}
#endif

#if not KICKOS_ARCH_SWITCH_LANDS_LATER
// Every device interrupt's exit asks, and with nothing owed it must cost no lock: on a kernel
// over several cores that lock is the cross-core one.
TEST(KTime, an_interrupt_exit_with_no_landing_owed_takes_no_lock)
{
    reset();
    uint32_t const saves = g_irq_saves;
    ktime_land_at_isr_exit();
    EXPECT_EQ(g_irq_saves, saves) << "the exit took the lock with no landing owed";
    EXPECT_EQ(g_arms, 0u);

    g_now = 5000000;
    g_slice = g_now + 2 * KICKOS_TIMER_MIN_DELTA_NS;
    kernel().switch_landing_owed[kickos_kernel_core()] = 1;
    ktime_land_at_isr_exit();
    EXPECT_EQ(kernel().switch_landing_owed[kickos_kernel_core()], 0u);
    EXPECT_EQ(g_armed, g_slice) << "an owed landing was not settled at the exit";
}
#endif
