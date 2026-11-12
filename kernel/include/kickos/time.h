// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Tickless time: monotonic clock, one absolute-deadline delta list per kernel core, and per
// core a one-shot next-event timer armed for min(nearest deadline on that core's list,
// running-RR slice expiry) with a minimum-delta guard. A core with nothing time-pending has
// its timer disarmed (zero timer interrupts).

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

    // The wall clock in Unix-epoch nanoseconds (kernel/time/realtime.cc), and its set: 0, or
    // -KOS_EPERM where `caller` lacks AUTH_REALTIME.
    uint64_t realtime_now();
    int32_t realtime_set(Thread* caller, uint64_t unix_ns);

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
    // nothing. `t` is the calling core's current thread, parking in the caller's span.
    void ktime_deadline_arm(Thread* t, uint32_t timeout_us, Held held);

    // A timed wait's limit as KOS_SYS_MUTEX_LOCK and KOS_SYS_WORD_WAIT take it (abi.h,
    // KOS_TIMEOUT_REALTIME). A bare timeout converts to one.
    struct WaitLimit
    {
        uint32_t lo = KOS_TIMEOUT_NONE;
        uint32_t hi = 0;

        constexpr WaitLimit(uint32_t timeout_us) : lo(timeout_us)
        {
        }

        constexpr WaitLimit(uint32_t limit_lo, uint32_t limit_hi) : lo(limit_lo), hi(limit_hi)
        {
        }

        static constexpr WaitLimit realtime(uint64_t unix_ns)
        {
            return WaitLimit(static_cast<uint32_t>(unix_ns),
                             static_cast<uint32_t>(unix_ns >> 32) | KOS_TIMEOUT_REALTIME);
        }

        constexpr bool valid() const
        {
            return hi == 0u or (hi & KOS_TIMEOUT_REALTIME) != 0u;
        }

        constexpr bool on_wall() const
        {
            return (hi & KOS_TIMEOUT_REALTIME) != 0u;
        }

        constexpr uint64_t unix_ns() const
        {
            return (static_cast<uint64_t>(hi & ~KOS_TIMEOUT_REALTIME) << 32) | lo;
        }
    };

    // Whether `limit` allows no wait at all: a timeout of 0, or a realtime deadline the wall
    // clock has reached.
    bool ktime_limit_passed(WaitLimit limit);
    // ktime_deadline_arm for any valid limit. A realtime one is kept on the monotonic timeline,
    // marked, and moved by every set of the wall clock while it is queued.
    void ktime_limit_arm(Thread* t, WaitLimit limit, Held held);

    // Queue `t` for the monotonic `deadline_ns`, already floored, marked as a realtime deadline
    // where `wall`.
    void ktime_deadline_arm_at(Thread* t, uint64_t deadline_ns, bool wall, Held held);
    // Move every marked deadline by `by_ns`, floored at now, on every core's delta list. Rearms
    // this core's timer if its own list moved, and asks each peer whose list moved for a
    // dispatch, which re-arms it.
    void ktime_wall_shift(int64_t by_ns, Held held);
    // The marked deadline for wall time `unix_ns`, keyed to this node's view of the wall clock,
    // queued for `t` (kernel/time/realtime.cc).
    void realtime_deadline_arm(Thread* t, uint64_t unix_ns, Held held);
    // Where KICKOS_AMP_CLOCK_SHARED: key this node's marked deadlines to the partition's newest
    // wall clock. The doorbell service calls it, a peer's set ringing every other node.
    void realtime_rekey(Held held);

    // Drop `t`'s deadline, if it has one. AT AN UNPARK AND NOWHERE ELSE: a pop is not
    // necessarily an unpark, and a park-to-park migration must keep its deadline.
    void ktime_deadline_cancel(Thread* t, Held held);

    // Recompute and (re)arm this core's one-shot timer from this core's delta list. Call after
    // any change that can affect the earliest deadline (new sleeper, context switch/RR slice,
    // wake). `incoming` is the thread this core is about to run, which the switch path holds
    // BEFORE it is seated; every other caller passes the thread already running.
    void ktime_rearm(Thread const* incoming, Held held);

    // Program the one-shot to fire for nothing, and forget what it held. The kernel is the
    // sole authority for that value, so every path that leaves the comparator describing
    // something other than what was last armed comes through here.
    void ktime_disarm(Held held);

    // Settles the landing a booking left owed: stamps the landed thread's slice and rearms the
    // comparator for it. A no-op when none is owed.
    void ktime_land(Held held);

    // Where the arch performs a switch booked in an interrupt at that interrupt's exit, the end of
    // the kernel's handler is its landing; elsewhere the arch's own hook lands it. Takes the lock
    // only when a landing is owed.
    void ktime_land_at_isr_exit();

    void ktime_on_timer();
}

#endif
