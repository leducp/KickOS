// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The counting semaphore on a user word: a unit taken and banked by compare-and-swap, the kernel
// entered only to wait for one or to hand one to a waiter.

#include <kickos/sys.h>
#include <kickos/sys/user_cas.h>
#include <kickos/arch/arch.h>

namespace
{
    int word_wait(uint32_t* word, uint32_t expected, int64_t ns, uint32_t kind)
    {
        uint64_t const limit = kos_limit_word(ns);
        return static_cast<int>(arch_syscall(KOS_SYS_WORD_WAIT, reinterpret_cast<uintptr_t>(word),
                                             static_cast<uintptr_t>(expected), kos_u64_lo(limit),
                                             kos_u64_hi(limit), static_cast<uintptr_t>(kind)));
    }
}

extern "C"
{

int kos_word_wait(uint32_t* word, uint32_t expected, kos_monotonic_t until)
{
    return word_wait(word, expected, until.ns, KOS_LIMIT_MONOTONIC);
}

int kos_word_wait_realtime(uint32_t* word, uint32_t expected, kos_realtime_t until)
{
    return word_wait(word, expected, until.ns, KOS_LIMIT_REALTIME);
}

int kos_word_wake(uint32_t* word, uint32_t n, uint32_t units)
{
    return static_cast<int>(arch_syscall(KOS_SYS_WORD_WAKE, reinterpret_cast<uintptr_t>(word),
                                         static_cast<uintptr_t>(n),
                                         static_cast<uintptr_t>(units), 0, 0));
}

int kos_sem_init(kos_sem_t* s, uint32_t initial)
{
    if (initial > static_cast<uint32_t>(KOS_SEM_COUNT_MAX))
    {
        return -KOS_EINVAL;
    }
    __atomic_store_n(&s->word, initial, __ATOMIC_RELEASE);
    return 0;
}

int kos_sem_wait(kos_sem_t* s, kos_monotonic_t until)
{
    uint32_t v = __atomic_load_n(&s->word, __ATOMIC_RELAXED);
    while (true)
    {
        if ((v & ~KOS_WORD_WAITERS) != 0u)
        {
            uint32_t const seen = kickos_user_cas(&s->word, v, v - 1u);
            if (seen == v)
            {
                return 0;
            }
            v = seen;
            continue;
        }
        // A poll leaves the word as it found it, with no waiters bit for a post to enter for.
        if (kos_limit_word(until.ns) == 0u)
        {
            return -KOS_EAGAIN;
        }
        uint32_t const want = v | KOS_WORD_WAITERS;
        if (want != v)
        {
            if (until.ns != KOS_NEVER.ns and until.ns <= kos_monotonic_now().ns)
            {
                return -KOS_ETIMEDOUT;
            }
            uint32_t const seen = kickos_user_cas(&s->word, v, want);
            if (seen != v)
            {
                v = seen;
                continue;
            }
        }
        int const rc = kos_word_wait(&s->word, want, until);
        if (rc == 1)
        {
            return 0;
        }
        if (rc < 0 and rc != -KOS_EAGAIN)
        {
            return rc;
        }
        // Beaten by a wake that cleared the bit, or woken with no unit: read the count again.
        v = __atomic_load_n(&s->word, __ATOMIC_RELAXED);
    }
}

int kos_sem_post(kos_sem_t* s)
{
    uint32_t v = __atomic_load_n(&s->word, __ATOMIC_RELAXED);
    while ((v & KOS_WORD_WAITERS) == 0u)
    {
        if (v == static_cast<uint32_t>(KOS_SEM_COUNT_MAX))
        {
            return -KOS_EOVERFLOW;
        }
        uint32_t const seen = kickos_user_cas(&s->word, v, v + 1u);
        if (seen == v)
        {
            return 0;
        }
        v = seen;
    }
    int const rc = kos_word_wake(&s->word, 1u, 1u);
    if (rc < 0)
    {
        return rc;
    }
    return 0;
}
}
