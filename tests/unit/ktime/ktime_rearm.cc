// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Host gate on the ONE property every arch one-shot backend depends on: the
// deadline ktime_rearm hands arch_timer_arm must be a function of what is
// pending, and of nothing else; in particular, not of when the call happens.
//
// A backend either dedups an arm by comparing the requested deadline against the one
// already programmed (arch/arm/common: g_armed_deadline_ns; arch/rx/rxv3:
// g_rx_armed_ns), or writes an absolute compare that is idempotent (CLINT mtimecmp,
// POSIX TIMER_ABSTIME), or converts a delta from the clock it reads on the spot
// (arch/xtensa/lx6: CCOMPARE0). A rearm that re-derives its value from the current
// clock defeats every one of those: ktime_rearm runs on EVERY context switch, so a
// moving value restarts the countdown before it can reach the compare and the sleeper
// is held off for as long as the switches keep coming.
//
// The armed deadline is arch-internal, and this gate links the REAL kernel/time/time.cc
// against a fake clock and a recording timer, so it reads the exact value handed to
// arch_timer_arm.

#include <kickos/time.h>
#include <kickos/sched.h>
#include <kickos/instance.h>
#include <kickos/arch/arch.h>

#include <stdio.h>
#include <stdlib.h>

#include <gtest/gtest.h>

namespace
{
    uint64_t g_now = 0;            // fake monotonic clock, advanced only by the test
    uint64_t g_armed = UINT64_MAX; // last deadline handed to arch_timer_arm
    uint32_t g_arms = 0;
    uint32_t g_disarms = 0;
}

// --- the seam under the unit: a fake clock and a recording timer ---
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
#if defined(KICKOS_MULTI_INSTANCE) && KICKOS_MULTI_INSTANCE
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

    // ktime_on_timer delegates every endpoint park's unwind to park.cc, which this gate does
    // not link. Reaching this means an arm staged a park the gate cannot model, so abort for
    // the same reason as kpanic above.
    void endpoint_wait_abort(Thread*, intptr_t)
    {
        fprintf(stderr, "endpoint_wait_abort: no endpoint layer in this gate\n");
        abort();
    }

    namespace sched
    {
        // The seam reports "no timed event" and parking is a no-op, which leaves the
        // sleeper on the sleepq for an arm to read.
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
}

// A deadline inside the min-delta window is the one case where a rearm is tempted to
// substitute now+min-delta for it.
TEST(KTime, deadline_is_stable_inside_the_window)
{
    reset();
    uint64_t const deadline = 1000000; // 1 ms
    park(deadline);

    g_now = deadline - KICKOS_TIMER_MIN_DELTA_NS / 4; // well inside the window
    ktime_rearm();
    uint64_t const first = g_armed;
    EXPECT_EQ(first, deadline) << "first arm inside the window is the parked deadline";

    // What a context switch does: same sleepq, later clock. Every backend's dedup
    // compares THIS value against the one above.
    for (uint32_t i = 0; i < 8; i++)
    {
        g_now += KICKOS_TIMER_MIN_DELTA_NS / 8;
        ktime_rearm();
        EXPECT_EQ(g_armed, first) << "rearm inside the window moved the deadline";
    }
}

// Same, once the deadline is already due: a due deadline fires immediately on every backend
// (the delta ones floor at one tick, the absolute compares are already behind), so what it
// must not become is a fresh future deadline.
TEST(KTime, due_deadline_is_not_pushed_into_the_future)
{
    reset();
    uint64_t const deadline = 1000000;
    park(deadline);
    g_now = deadline + 1;
    ktime_rearm();
    EXPECT_EQ(g_armed, deadline) << "a due deadline was rearmed into the future";

    g_now += 5 * KICKOS_TIMER_MIN_DELTA_NS;
    ktime_rearm();
    EXPECT_EQ(g_armed, deadline) << "a due deadline drifted with the clock";
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

// Tickless: nothing pending must disarm, not arm something far away.
TEST(KTime, empty_disarms)
{
    reset();
    g_now = 1234;
    ktime_rearm();
    EXPECT_EQ(g_disarms, 1u) << "an empty sleepq did not disarm";
    EXPECT_EQ(g_arms, 0u) << "an empty sleepq armed instead of disarming";
}
