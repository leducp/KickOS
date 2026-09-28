// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What the tasks of this example exchange. The formats between user tasks are the user's, so
// they live here and not in any KickOS header.

#ifndef KICKOS_EXAMPLES_COMPOSITION_SAMPLE_H
#define KICKOS_EXAMPLES_COMPOSITION_SAMPLE_H

#include <stdint.h>

// The one message /svc/sensor answers with.
struct sample
{
    uint32_t value;
};

// /shm/history: the sensor's last readings, which the health checker reads after a death to
// log what the sensor saw last.
//
// The init zeroes a declared shared region once, when the system starts, and never again: a
// restarted task finds it as its previous instance left it. All zeroes is therefore the
// initial state here, and a restarted writer must cope with a region its predecessor died
// halfway through writing.
//
// One writer, any number of readers, under a sequence lock: `seq` is odd while an append is in
// progress, and a reader that sees it change across its read discards what it read. Every
// field is accessed atomically, so a concurrent read is never a data race.
struct history
{
    uint32_t seq;
    uint32_t next; // readings written; the latest is at (next - 1) % HISTORY_LEN
    uint32_t value[14];
};
constexpr uint32_t HISTORY_LEN = sizeof(history::value) / sizeof(history::value[0]);

inline void history_append(history* h, uint32_t v)
{
    // A writer that died mid-append left `seq` odd; step past it, so a reader holding the even
    // value from before that append still sees a change.
    uint32_t const s = __atomic_load_n(&h->seq, __ATOMIC_RELAXED);
    uint32_t begin = s + 1u;
    if ((s & 1u) != 0)
    {
        begin = s + 2u;
    }
    __atomic_store_n(&h->seq, begin, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    uint32_t const n = __atomic_load_n(&h->next, __ATOMIC_RELAXED);
    __atomic_store_n(&h->value[n % HISTORY_LEN], v, __ATOMIC_RELAXED);
    __atomic_store_n(&h->next, n + 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&h->seq, begin + 1u, __ATOMIC_RELEASE);
}

// The latest reading, or false when none has been written or no consistent copy was taken in
// a few attempts, which a writer appending at ten readings a second makes rare. Bounded, so a
// writer that died mid-append costs the reader a false answer, never a hang.
inline bool history_latest(history const* h, uint32_t* out)
{
    for (int attempt = 0; attempt < 4; attempt++)
    {
        uint32_t const s1 = __atomic_load_n(&h->seq, __ATOMIC_ACQUIRE);
        if ((s1 & 1u) != 0)
        {
            continue;
        }
        uint32_t const n = __atomic_load_n(&h->next, __ATOMIC_RELAXED);
        uint32_t v = 0;
        if (n != 0)
        {
            v = __atomic_load_n(&h->value[(n - 1u) % HISTORY_LEN], __ATOMIC_RELAXED);
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&h->seq, __ATOMIC_RELAXED) == s1)
        {
            *out = v;
            return n != 0;
        }
    }
    return false;
}

#endif
