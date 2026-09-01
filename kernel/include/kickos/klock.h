// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The kernel lock's nesting bracket: one acquisition depth per core, so the cross-core lock
// is taken as the depth rises from zero and released as it returns to it.
//
// Every body takes THIS CORE'S INTERRUPT MASK AS ALREADY APPLIED: the lock must be taken
// after the mask and released before it comes back, or a handler entered on a core holding
// the lock spins against its own release.

#ifndef KICKOS_KLOCK_H
#define KICKOS_KLOCK_H

#include <stdint.h>

#include <kickos/arch/arch.h>

namespace kickos
{
#if KICKOS_KERNEL_CORES > 1
    void klock_enter(void);
    void klock_leave(void);

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
#endif
}

#endif
