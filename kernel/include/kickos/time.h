// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Tickless time: monotonic clock, an absolute-deadline delta list, and a
// single one-shot next-event timer armed for min(nearest sleep deadline,
// running-RR slice expiry) with a minimum-delta guard. Pure-FIFO with nothing
// time-pending leaves the timer disarmed (zero timer interrupts).

#ifndef KICKOS_TIME_H
#define KICKOS_TIME_H

#include <stdint.h>

#include <kickos/config/board.h>
#include <kickos/sys/abi.h> // kos_pstate_t
#include <kickos/held.h>

namespace kickos
{
    struct Thread;

    // Coherently retune the core clock to `target`. Returns the LANDED core Hz, or 0 if the
    // chip cannot change its clock or a userspace driver owns the console. Privileged, thread
    // context.
    uint64_t cpu_clock_set(kos_pstate_t target);
    uint64_t ktime_now(); // monotonic nanoseconds

    // Sleep the current thread until absolute `deadline_ns` (monotonic). Blocks.
    void ktime_sleep_until(uint64_t deadline_ns);
    void ktime_sleep_ns(uint64_t ns);

    // `deadline_ns`, moved no sooner than `now` plus the timer's min delta. Applied where a
    // deadline is born, against one clock reading, and never re-derived later (invariant
    // timer-min-delta-guard).
    inline uint64_t ktime_floored(uint64_t now, uint64_t deadline_ns)
    {
        uint64_t const floor = now + KICKOS_TIMER_MIN_DELTA_NS;
        if (deadline_ns < floor)
        {
            return floor;
        }
        return deadline_ns;
    }

    // Give `t` a deadline `timeout_us` microseconds out and put it on the delta list, so a
    // park on some OTHER queue can be unwound when the deadline passes. KOS_TIMEOUT_NONE arms
    // nothing.
    void ktime_deadline_arm(Thread* t, uint32_t timeout_us, Held held);

    // Drop `t`'s deadline, if it has one. AT AN UNPARK AND NOWHERE ELSE: a pop is not
    // necessarily an unpark, and a park-to-park migration must keep its deadline.
    void ktime_deadline_cancel(Thread* t, Held held);

    // Recompute and (re)arm the one-shot timer. Call after any change that can
    // affect the earliest deadline (new sleeper, context switch/RR slice, wake).
    // `incoming` is the thread this core is about to run, which the switch path holds
    // BEFORE it is seated; every other caller passes the thread already running.
    void ktime_rearm(Thread const* incoming, Held held);

    // Program the one-shot to fire for nothing, and forget what it held. The kernel is the
    // sole authority for that value, so every path that leaves the comparator describing
    // something other than what was last armed comes through here.
    void ktime_disarm(Held held);

    void ktime_on_timer();

#if defined(KICKOS_ENABLE_SELFTEST) && KICKOS_KERNEL_CORES > 1
    // One bit per core whose OWN slice timer took a thread off it: set only where
    // sched::tick_rr inside ktime_on_timer changed the running thread, so a cross-core
    // reschedule or a device wake sets nothing.
    uint32_t ktime_slice_preempt_cores();
#endif
}

#endif
