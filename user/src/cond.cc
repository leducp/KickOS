// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The condition variable on a user word: a sequence every signal and broadcast advances, waited
// on through the kernel's plain word pair.

#include <kickos/sys.h>
#include <kickos/sys/user_cas.h>

namespace
{
    constexpr uint32_t SEQUENCE = ~static_cast<uint32_t>(KOS_WORD_WAITERS);

    // A cancelled waiter asks at the kernel's entry, which ends it there: a free mutex taken by
    // compare-and-swap on its way out would die held.
    int retake(kos_mutex_t* m, int waited)
    {
        int rc = 0;
        if (waited == -KOS_ECANCELED)
        {
            rc = kickos_mutex_lock_entry(m, KOS_TIMEOUT_NONE);
        }
        else
        {
            rc = kos_mutex_lock(m, KOS_TIMEOUT_NONE);
        }
        while (rc == -KOS_ECANCELED)
        {
            rc = kickos_mutex_lock_entry(m, KOS_TIMEOUT_NONE);
        }
        if (rc != 0)
        {
            return rc;
        }
        return waited;
    }

    // The bump keeps the waiters bit, which only the kernel clears.
    int advance(kos_cond_t* c, uint32_t n)
    {
        uint32_t v = __atomic_load_n(&c->word, __ATOMIC_RELAXED);
        while (true)
        {
            uint32_t const next = ((v + 1u) & SEQUENCE) | (v & KOS_WORD_WAITERS);
            uint32_t const seen = kickos_user_cas(&c->word, v, next);
            if (seen == v)
            {
                break;
            }
            v = seen;
        }
        if ((v & KOS_WORD_WAITERS) == 0u)
        {
            return 0;
        }
        int const rc = kos_word_wake(&c->word, n, 0u);
        if (rc < 0)
        {
            return rc;
        }
        return 0;
    }

    // The wait toward `limit`: relative microseconds, or a wall-clock deadline where `wall`.
    int wait(kos_cond_t* c, kos_mutex_t* m, bool wall, uint64_t limit)
    {
        // Read under the mutex: a signal sent after the release moves the sequence off it.
        uint32_t const seq = __atomic_load_n(&c->word, __ATOMIC_RELAXED) & SEQUENCE;
        int const released = kos_mutex_unlock(m);
        if (released != 0)
        {
            return released;
        }
        uint32_t const want = seq | KOS_WORD_WAITERS;
        uint32_t const seen = kickos_user_cas(&c->word, seq, want);
        int waited = 0;
        if (seen == seq or seen == want)
        {
            if (wall)
            {
                waited = kos_word_wait_realtime(&c->word, want, limit);
            }
            else
            {
                waited = kos_word_wait(&c->word, want, static_cast<uint32_t>(limit));
            }
        }
        if (waited == 1 or waited == -KOS_EAGAIN)
        {
            waited = 0;
        }
        return retake(m, waited);
    }
}

extern "C"
{

int kos_cond_wait(kos_cond_t* c, kos_mutex_t* m, uint32_t timeout_us)
{
    return wait(c, m, false, timeout_us);
}

int kos_cond_wait_realtime(kos_cond_t* c, kos_mutex_t* m, uint64_t unix_ns)
{
    return wait(c, m, true, unix_ns);
}

int kos_cond_signal(kos_cond_t* c)
{
    return advance(c, 1u);
}

int kos_cond_broadcast(kos_cond_t* c)
{
    return advance(c, 0xFFFFFFFFu);
}
}
