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
    void ktime_init();

    // Coherently retune the core clock to `target` (the MECHANISM seam; policy lives
    // in a future userspace power manager). Returns the LANDED core Hz -- 0 if the chip
    // cannot change its clock, or if a userspace driver owns the console. Privileged,
    // thread context. See kernel/time/clock_select.cc + docs/design-m3-clock-select.md.
    uint32_t cpu_clock_set(kos_pstate_t target);
    uint64_t ktime_now(); // monotonic nanoseconds

    // Sleep the current thread until absolute `deadline_ns` (monotonic). Blocks.
    void ktime_sleep_until(uint64_t deadline_ns);
    // Convenience: sleep for a relative duration.
    void ktime_sleep_ns(uint64_t ns);

    // Recompute and (re)arm the one-shot timer. Called after any change that can
    // affect the earliest deadline (new sleeper, context switch/RR slice, wake).
    void ktime_rearm();

    // The timer-expiry ISR body (invoked by arch via kickos_isr_timer()).
    void ktime_on_timer();
}

#endif
