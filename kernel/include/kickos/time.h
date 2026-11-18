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

    // The realtime clock in Unix-epoch nanoseconds (kernel/time/realtime.cc), and its set: 0,
    // or -KOS_EPERM where `caller` lacks AUTH_REALTIME.
    uint64_t realtime_now();
    int32_t realtime_set(Thread* caller, uint64_t unix_ns);

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

    // A wait's limit as the trap carries it (abi.h): a word, KOS_LIMIT_NONE for none, read
    // against the clock its kind names. A bare monotonic deadline converts to one.
    struct WaitLimit
    {
        uint64_t word = KOS_LIMIT_NONE;
        uint32_t kind = KOS_LIMIT_MONOTONIC;

        constexpr WaitLimit(uint64_t deadline) : word(deadline)
        {
        }

        constexpr WaitLimit(uint64_t limit, uint32_t limit_kind) : word(limit), kind(limit_kind)
        {
        }

        constexpr bool none() const
        {
            return word == KOS_LIMIT_NONE;
        }
    };

    // Whether a wait takes a limit of `kind`: RELATIVE is a sleep's alone.
    constexpr bool ktime_limit_kind_ok(uintptr_t kind)
    {
        return kind == KOS_LIMIT_MONOTONIC or kind == KOS_LIMIT_REALTIME;
    }
    // ktime_limit_passed and ktime_limit_arm for a limit that is not none.
    bool ktime_limit_word_passed(uint64_t word, uint32_t kind);
    void ktime_limit_word_arm(Thread* t, uint64_t word, uint32_t kind, Held held);
    // Whether `limit` allows no wait at all: a deadline its clock has read.
    inline bool ktime_limit_passed(WaitLimit const& limit)
    {
        if (limit.none())
        {
            return false;
        }
        return ktime_limit_word_passed(limit.word, limit.kind);
    }
    // Give `t` the deadline `limit` names and put it on the delta list, so a park on some
    // OTHER queue can be unwound when it passes; no limit arms nothing. A realtime one is kept on
    // the monotonic timeline, marked, and moved by every set of the realtime clock while it is
    // queued. `t` is the calling core's current thread, parking in the caller's span.
    inline void ktime_limit_arm(Thread* t, WaitLimit const& limit, Held held)
    {
        if (limit.none())
        {
            return;
        }
        ktime_limit_word_arm(t, limit.word, limit.kind, held);
    }
    // The limit word (lo, hi).
    inline uint64_t ktime_limit_word(uintptr_t lo, uintptr_t hi)
    {
        return kos_u64_join(static_cast<uint32_t>(lo), static_cast<uint32_t>(hi));
    }

    // Park the current thread for `limit`, which may be RELATIVE: nanoseconds from now. A
    // passed deadline or a relative 0 yields and never parks. -KOS_EINVAL for an unknown kind,
    // else 0.
    int ktime_sleep(uint64_t limit, uintptr_t kind);

    // Queue `t` for the monotonic `deadline_ns`, already floored, marked as a realtime deadline
    // where `wall`.
    void ktime_deadline_arm_at(Thread* t, uint64_t deadline_ns, bool wall, Held held);
    // Move every marked deadline by `by_ns`, floored at now, on every core's delta list. Rearms
    // this core's timer if its own list moved, and asks each peer whose list moved for a
    // dispatch, which re-arms it.
    void ktime_wall_shift(int64_t by_ns, Held held);
    // The marked deadline for realtime `unix_ns`, keyed to this node's view of the realtime
    // clock, queued for `t` (kernel/time/realtime.cc).
    void realtime_deadline_arm(Thread* t, uint64_t unix_ns, Held held);
    // Where KICKOS_AMP_CLOCK_SHARED: key this node's marked deadlines to the partition's newest
    // realtime clock. The doorbell service calls it, a peer's set ringing every other node.
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
