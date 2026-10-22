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
    uint32_t us_until(uint64_t deadline_ns)
    {
        uint64_t const now = kos_clock_now();
        if (now >= deadline_ns)
        {
            return 0u;
        }
        return static_cast<uint32_t>((deadline_ns - now) / 1000u);
    }
}

extern "C"
{

int kos_word_wait(uint32_t* word, uint32_t expected, uint32_t timeout_us)
{
    return static_cast<int>(arch_syscall(KOS_SYS_WORD_WAIT, reinterpret_cast<uintptr_t>(word),
                                         static_cast<uintptr_t>(expected),
                                         static_cast<uintptr_t>(timeout_us), 0));
}

int kos_word_wake(uint32_t* word, uint32_t n, uint32_t units)
{
    return static_cast<int>(arch_syscall(KOS_SYS_WORD_WAKE, reinterpret_cast<uintptr_t>(word),
                                         static_cast<uintptr_t>(n),
                                         static_cast<uintptr_t>(units), 0));
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

int kos_sem_wait(kos_sem_t* s, uint32_t timeout_us)
{
    uint32_t v = __atomic_load_n(&s->word, __ATOMIC_RELAXED);
    uint32_t wait_us = timeout_us;
    uint64_t deadline_ns = 0;
    bool started = false;
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
        if (started and timeout_us != KOS_TIMEOUT_NONE)
        {
            wait_us = us_until(deadline_ns);
        }
        if (wait_us == 0u)
        {
            return -KOS_ETIMEDOUT;
        }
        // Read before the waiters bit is set, so a kill landing at the clock's own entry leaves
        // the word as it found it.
        if (not started)
        {
            started = true;
            if (timeout_us != KOS_TIMEOUT_NONE)
            {
                deadline_ns = kos_clock_now() + static_cast<uint64_t>(timeout_us) * 1000u;
            }
        }
        uint32_t const want = v | KOS_WORD_WAITERS;
        if (want != v)
        {
            uint32_t const seen = kickos_user_cas(&s->word, v, want);
            if (seen != v)
            {
                v = seen;
                continue;
            }
        }
        int const rc = kos_word_wait(&s->word, want, wait_us);
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
