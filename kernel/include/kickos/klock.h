// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Per-core kernel-lock nesting. Acquire when depth changes from zero and
// release when it returns to zero.
// Call with local interrupts masked. Restore them only after release, or
// an interrupt handler can deadlock on the lock held by its own core.

#ifndef KICKOS_KLOCK_H
#define KICKOS_KLOCK_H

#include <stdint.h>

#include <kickos/arch/arch.h>
#include <kickos/debug.h>

namespace kickos
{
#if KICKOS_KERNEL_CORES > 1
    void klock_enter(void);
    void klock_leave(void);

#if KICKOS_DEBUG
    // Includes the switch window: klock_detach clears depth without releasing the lock.
    bool klock_exclusion_held(void);
    // The bracket depth alone, so zero while a booked swap is still owed the lock.
    uint32_t klock_depth(void);
#endif

    // detach takes the depth off the core and LEAVES THE LOCK HELD: kickos_switch_unlock
    // releases it once the swap has parked the outgoing frame. attach puts the depth back,
    // acquiring only when the swap already ran.
    uint32_t klock_detach(void);
    void klock_attach(uint32_t depth);

    // Drops the depth and the lock outright, for a caller that never returns to destroy its
    // own bracket.
    void klock_drop(void);

    // Asks every core named in `cores` to reschedule. THE ASK IS PUBLISHED AS STATE AHEAD OF
    // THE RAISE THAT CARRIES IT, so a consumer absorbing that raise without entering a
    // scheduler leaves the ask standing for the next release or dispatch.
    void klock_resched_ask(uint32_t cores);

    // The ask a core owes ITSELF. klock_resched_ask strips the caller's bit, so a core cannot
    // reach its own cell through it. THE CELL ONLY: the raise is fired by whichever release
    // ends this core's lock span, and a second publisher here would put the ask ahead of a
    // raise nothing ordered it against.
    void klock_resched_self(void);
#else
    inline void klock_enter(void)
    {
    }
    inline void klock_leave(void)
    {
    }
    inline uint32_t klock_detach(void)
    {
        return 0;
    }
    inline void klock_attach(uint32_t)
    {
    }
    inline void klock_drop(void)
    {
    }
    inline void klock_resched_ask(uint32_t)
    {
    }
    inline void klock_resched_self(void)
    {
    }
#endif
}

// Assert the caller holds kernel exclusion. On single-core builds this is a no-op:
// exclusion uses the interrupt mask, which has no query interface.
#if KICKOS_KERNEL_CORES > 1 && KICKOS_DEBUG
#define KICKOS_ASSERT_EXCLUSION_HELD() KICKOS_DEBUG_ASSERT(::kickos::klock_exclusion_held())
#else
#define KICKOS_ASSERT_EXCLUSION_HELD() ((void)0)
#endif

#endif
