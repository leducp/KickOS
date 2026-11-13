// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The mutex on a user word: taken and released by compare-and-swap, the kernel entered only on
// contention.

#include <kickos/sys.h>
#include <kickos/sys/user_cas.h>
#include <kickos/arch/arch.h>

namespace
{
    int lock(kos_mutex_t* m, uint64_t limit, uint32_t kind)
    {
        uint32_t const self = kickos_self_lock_id();
        uint32_t const held = kickos_user_cas(&m->word, 0u, self);
        if (held == 0u)
        {
            return 0;
        }
        if ((held & ~KOS_WORD_WAITERS) == self)
        {
            return -KOS_EDEADLK;
        }
        return kickos_mutex_lock_entry(m, limit, kind);
    }
}

extern "C"
{

int kickos_mutex_lock_entry(kos_mutex_t* m, uint64_t limit, uint32_t kind)
{
    return static_cast<int>(arch_syscall(KOS_SYS_MUTEX_LOCK, reinterpret_cast<uintptr_t>(&m->word),
                                         kos_u64_lo(limit), kos_u64_hi(limit),
                                         static_cast<uintptr_t>(kind), 0));
}

int kos_mutex_lock(kos_mutex_t* m, kos_monotonic_t until)
{
    return lock(m, kos_limit_word(until.ns), KOS_LIMIT_MONOTONIC);
}

int kos_mutex_lock_realtime(kos_mutex_t* m, kos_realtime_t until)
{
    return lock(m, kos_limit_word(until.ns), KOS_LIMIT_REALTIME);
}

int kos_mutex_unlock(kos_mutex_t* m)
{
    uint32_t const self = kickos_self_lock_id();
    uint32_t const held = kickos_user_cas(&m->word, self, 0u);
    if (held == self)
    {
        return 0;
    }
    if ((held & ~KOS_WORD_WAITERS) != self)
    {
        return -KOS_EPERM;
    }
    return static_cast<int>(arch_syscall(KOS_SYS_MUTEX_UNLOCK,
                                         reinterpret_cast<uintptr_t>(&m->word), 0, 0, 0, 0));
}
}
