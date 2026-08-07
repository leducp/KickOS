// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Host gate on the ONE property every arch one-shot backend depends on: the
// deadline ktime_rearm hands arch_timer_arm must be a function of what is
// pending, and of nothing else; in particular, not of when the call happens.
//
// Every backend dedups an arm by comparing the requested deadline against the one
// already programmed (arch/arm/common: g_armed_deadline_ns; arch/rx/rxv3:
// g_rx_armed_ns) or by writing an absolute compare that is idempotent (CLINT
// mtimecmp, POSIX TIMER_ABSTIME). A rearm that re-derives its value from the
// current clock defeats all four shapes at once: ktime_rearm runs on EVERY context
// switch, so a moving value restarts the countdown before it can reach the compare
// and the sleeper is held off for as long as the switches keep coming.
//
// Host-only of necessity, not convenience: the armed deadline is arch-internal, so
// no on-target arm can read it: an on-target test can only time a wake and race
// the host scheduler for the answer. This links the REAL kernel/time/time.cc
// against a fake clock and a recording timer, so the reading is exact.

#include <kickos/time.h>
#include <kickos/sched.h>
#include <kickos/instance.h>
#include <kickos/arch/arch.h>

#include <stdio.h>
#include <stdlib.h>

namespace
{
    uint64_t g_now = 0;             // fake monotonic clock, advanced only by the test
    uint64_t g_armed = UINT64_MAX;  // last deadline handed to arch_timer_arm
    unsigned g_arms = 0;
    unsigned g_disarms = 0;
    int g_fails = 0;

    void check(bool ok, char const* what)
    {
        if (not ok)
        {
            printf("FAIL: %s\n", what);
            g_fails++;
        }
    }

    void check_eq(uint64_t got, uint64_t want, char const* what)
    {
        if (got != want)
        {
            printf("FAIL: %s: got %llu want %llu\n", what,
                   static_cast<unsigned long long>(got),
                   static_cast<unsigned long long>(want));
            g_fails++;
        }
    }
}

// --- The seam under the unit: a fake clock, a recording timer, no real threads ---
extern "C"
{
    arch_irq_state_t arch_irq_save(void) { return 0; }
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

    // Only referenced when the build carries telemetry: time.cc includes ktrace.h, whose
    // inline emitters call both. The sim-telem preset links them; without these stubs the
    // gate fails at LINK there while passing everywhere else.
    uint32_t arch_trace_now(void) { return 0; }
    void kickos_rtt_write_record_ch1(void const*, unsigned long) {}
}

namespace kickos
{
    namespace detail
    {
        Kernel g_instance;
    }

    // time.cc calls kpanic when ktime_on_timer finds a deadline under a park that cannot
    // time out. kpanic is noreturn: a stub that returns is undefined behaviour, and would
    // present as this gate passing through a kernel invariant violation.
    void kpanic(char const* msg)
    {
        fprintf(stderr, "kpanic: %s\n", msg);
        abort();
    }

    // ktime_on_timer delegates every endpoint park's unwind to the IPC layer, which this
    // gate does not link (it compiles time.cc alone against a fake clock). No arm here
    // stages an endpoint park, so reaching this is a test that staged something it cannot
    // model; abort rather than return, for the same reason as kpanic above.
    void endpoint_wait_timeout(Thread*)
    {
        fprintf(stderr, "endpoint_wait_timeout: no IPC layer in this gate\n");
        abort();
    }

    namespace sched
    {
        // No policy and no run set here: the unit under test is the deadline
        // arithmetic, so the scheduler seam reports "no timed event" and parking is
        // a no-op that leaves the thread on the sleepq for the test to inspect.
        uint64_t next_timed_event() { return UINT64_MAX; }
        Thread* current() { return kernel().current; }
        void block_current() {}
        void wake(Thread*) {}
        void tick_rr(uint64_t) {}
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
        kernel().current = &g_sleeper;
        g_sleeper.tnext = nullptr;
        g_sleeper.on_timer = false;
        g_armed = UINT64_MAX;
        g_arms = 0;
        g_disarms = 0;
    }

    void park(uint64_t deadline_ns)
    {
        g_sleeper.deadline_ns = deadline_ns;
        g_sleeper.on_timer = true;
        g_sleeper.tnext = nullptr;
        kernel().sleepq = &g_sleeper;
    }

    // The defect, stated as a test: a deadline inside the min-delta window is the
    // one case where a rearm is tempted to substitute now+min-delta for it.
    void t_deadline_is_stable_inside_the_window()
    {
        reset();
        uint64_t const deadline = 1000000; // 1 ms
        park(deadline);

        g_now = deadline - KICKOS_TIMER_MIN_DELTA_NS / 4; // well inside the window
        ktime_rearm();
        uint64_t const first = g_armed;
        check_eq(first, deadline, "first arm inside the window is the parked deadline");

        // What a context switch does: same sleepq, later clock. Every backend's dedup
        // compares THIS value against the one above.
        for (int i = 0; i < 8; i++)
        {
            g_now += KICKOS_TIMER_MIN_DELTA_NS / 8;
            ktime_rearm();
            check_eq(g_armed, first, "rearm inside the window moved the deadline");
        }
    }

    // Same, once the deadline is already due: the arch backends all floor the
    // programmed delta at one tick, so a past deadline is an immediate fire and needs
    // no help from the kernel: what it must not become is a fresh future deadline.
    void t_due_deadline_is_not_pushed_into_the_future()
    {
        reset();
        uint64_t const deadline = 1000000;
        park(deadline);
        g_now = deadline + 1;
        ktime_rearm();
        check_eq(g_armed, deadline, "a due deadline was rearmed into the future");

        g_now += 5 * KICKOS_TIMER_MIN_DELTA_NS;
        ktime_rearm();
        check_eq(g_armed, deadline, "a due deadline drifted with the clock");
    }

    // The min-delta floor is not dropped, only moved: it belongs where the deadline is
    // born, so that it is applied ONCE against ONE clock reading.
    void t_floor_is_applied_at_birth()
    {
        reset();
        g_now = 5000000;
        ktime_sleep_until(g_now); // a deadline already due at the moment of the call
        check(kernel().sleepq == &g_sleeper, "sleeper not parked");
        check_eq(g_sleeper.deadline_ns, g_now + KICKOS_TIMER_MIN_DELTA_NS,
                 "an already-due sleep was not floored at birth");

        reset();
        g_now = 5000000;
        ktime_sleep_ns(1); // 1 ns: below the floor, and not the sleep(0) yield case
        check(kernel().sleepq == &g_sleeper, "sub-floor sleeper not parked");
        check_eq(g_sleeper.deadline_ns, g_now + KICKOS_TIMER_MIN_DELTA_NS,
                 "a sub-min-delta sleep did not round up to the min slice");

        // And a deadline born above the floor is passed through untouched.
        reset();
        g_now = 5000000;
        ktime_sleep_ns(1000000);
        check_eq(g_sleeper.deadline_ns, g_now + 1000000, "a normal sleep was perturbed");
    }

    // Tickless: nothing pending must disarm, not arm something far away.
    void t_empty_disarms()
    {
        reset();
        g_now = 1234;
        ktime_rearm();
        check(g_disarms == 1 and g_arms == 0, "an empty sleepq did not disarm");
    }
}

int main()
{
    t_deadline_is_stable_inside_the_window();
    t_due_deadline_is_not_pushed_into_the_future();
    t_floor_is_applied_at_birth();
    t_empty_disarms();
    if (g_fails != 0)
    {
        printf("ktime_rearm: %d failure(s)\n", g_fails);
        return 1;
    }
    printf("ktime_rearm: ok\n");
    return 0;
}
