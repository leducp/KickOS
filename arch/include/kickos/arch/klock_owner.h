// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Which core holds the kernel lock, under KICKOS_DEBUG alone.
//
// The ticket counters cannot answer this: g_next_ticket differing from g_now_serving says only
// that some core has an outstanding ticket, so a release taken on a core holding nothing reads
// as legal, advances the turn onto a ticket nobody drew, and every later draw spins for good.
//
// One word per core, written by that core alone. A single word naming the holder would answer
// the same question in fewer bytes, but it would be written by two cores exactly when the
// protocol it checks has already let two cores in, which is the only time it is read.
//
// At KICKOS_DEBUG=0 the two calls below expand to nothing and no cell is allocated, so
// arch_kernel_unlock stays the leaf tests/static/trap_redzone_roots.txt prices it as.

#ifndef KICKOS_ARCH_KLOCK_OWNER_H
#define KICKOS_ARCH_KLOCK_OWNER_H

#include <kickos/arch/arch.h>

#include <stdint.h>

#if KICKOS_KERNEL_CORES > 1 && defined(KICKOS_DEBUG) && KICKOS_DEBUG

namespace kickos::klock
{
    // Defined in arch/common/doorbell_protocol.cc, beside the other cells a shared kernel's
    // cores keep about each other.
    extern uint32_t g_owner[KICKOS_NUM_CORES];

    // After the turn has come, never at the draw: a ticket grants nothing, and a word set at
    // the draw would claim the lock throughout the wait for it.
    inline void owner_take(void)
    {
        g_owner[arch_cpu_id()] = 1u;
    }

    // Before the turn is advanced. Cleared after the release, the word would still claim the
    // lock over the interval in which the next holder is already running under it.
    inline void owner_drop(void)
    {
        g_owner[arch_cpu_id()] = 0u;
    }
}

#else

namespace kickos::klock
{
    inline void owner_take(void)
    {
    }

    inline void owner_drop(void)
    {
    }
}

#endif

#endif
