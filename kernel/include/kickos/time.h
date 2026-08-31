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

#include <kickos/sys/abi.h> // kos_pstate_t

namespace kickos
{
    struct Thread; // kickos/thread.h: the TCB a deadline is armed on

    void ktime_init();

    // Coherently retune the core clock to `target` (the MECHANISM seam; policy lives
    // in a future userspace power manager). Returns the LANDED core Hz, or 0 if the chip
    // cannot change its clock or a userspace driver owns the console. Privileged, thread
    // context. See kernel/time/clock_select.cc.
    uint32_t cpu_clock_set(kos_pstate_t target);
    uint64_t ktime_now(); // monotonic nanoseconds

    // Sleep the current thread until absolute `deadline_ns` (monotonic). Blocks.
    void ktime_sleep_until(uint64_t deadline_ns);
    // Convenience: sleep for a relative duration.
    void ktime_sleep_ns(uint64_t ns);

    // Give `t` a deadline `timeout_us` microseconds out and put it on the delta list, so a
    // park on some OTHER queue can be unwound when the deadline passes. The min-delta floor
    // is applied here, at the deadline's birth, and must never be re-derived later
    // (invariant timer-min-delta-guard). Caller holds IrqLock.
    void ktime_deadline_arm(Thread* t, uint32_t timeout_us);

    // Drop `t`'s deadline, if it has one. AT AN UNPARK AND NOWHERE ELSE: a pop is not
    // necessarily an unpark, and a park-to-park migration must keep its deadline. Caller
    // holds IrqLock.
    void ktime_deadline_cancel(Thread* t);

    // Recompute and (re)arm the one-shot timer. Call after any change that can
    // affect the earliest deadline (new sleeper, context switch/RR slice, wake).
    void ktime_rearm();

    // The timer-expiry ISR body.
    void ktime_on_timer();
}

#endif
