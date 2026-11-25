// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The KickOS threads model's operations (gthr-kickos.h), which the C++ runtime calls for its
// mutexes, condition variables, keys and threads: the mutex word, kos_cond_t, key values in the
// thread's cell and kos_thread_create. Each answers 0 or a positive errno value. A deadline is
// on CLOCK_MONOTONIC, steady_clock's, where the call names it, else on CLOCK_REALTIME,
// system_clock's, and a realtime wait follows a set of the wall clock made during it. The once
// is gthr_once.cc's.

#include <kickos/config/pool_abi.h> // KICKOS_HANDLE_INDEX_BITS
#include <kickos/reent.h>
#include <kickos/sys.h>
#include <kickos/sys/trap.h>
#include <kickos/sys/user_cas.h>

#include <bits/gthr.h>
#include <errno.h>
#include <malloc.h>
#include <time.h>
#include <unistd.h>

namespace
{
    constexpr uint32_t KEY_FREE = 0;
    constexpr uint32_t KEY_USED = 1;
    constexpr uint32_t KEY_CLAIMED = 2;
    constexpr int KEY_PASSES = 4;
    // A key is its index plus KICKOS_THREAD_KEYS times its index's generation, which each create
    // advances: a thread's value counts for a key only when it was set under that generation, so
    // one a deleted key left behind is never the new key's.
    constexpr uint32_t KEY_GENERATIONS
        = static_cast<uint32_t>((uint64_t{1} << 32) / KICKOS_THREAD_KEYS);
    static_assert(uint64_t{KICKOS_THREAD_KEYS} * KEY_GENERATIONS - 1u <= UINT32_MAX,
                  "the largest key does not fit a __gthread_key_t");

    struct Key
    {
        uint32_t state;
        uint32_t gen;
        void (*dtor)(void*);
    };

    Key s_keys[KICKOS_THREAD_KEYS];

    uint32_t index_of(__gthread_key_t key)
    {
        return key % KICKOS_THREAD_KEYS;
    }

    uint32_t gen_of(__gthread_key_t key)
    {
        return key / KICKOS_THREAD_KEYS;
    }

    // The key's slot while the key is live, else null.
    Key* live(__gthread_key_t key)
    {
        Key* const k = &s_keys[index_of(key)];
        if (__atomic_load_n(&k->state, __ATOMIC_SEQ_CST) != KEY_USED
            or __atomic_load_n(&k->gen, __ATOMIC_SEQ_CST) != gen_of(key))
        {
            return nullptr;
        }
        return k;
    }

    constexpr uint32_t RUN_LIVE = 0;
    constexpr uint32_t RUN_RETURNED = 1;
    constexpr uint32_t RUN_DETACHED = 2;

    // A thread __kickos_gthr_create started. A joinable one's is on s_runs until its joiner frees
    // it. A detached one's is on s_detached until the thread frees it on return; one that never
    // returns leaves it until a create is handed a thread in its slot.
    struct Run
    {
        kos_thread_t handle;
        void* (*func)(void*);
        void* arg;
        void* ret;
        uint32_t state;
        Run* next;
    };

    kos_mutex_t s_runs_lock = KOS_MUTEX_INIT;
    Run* s_runs = nullptr;
    Run* s_detached = nullptr;

    constexpr uint32_t HANDLE_INDEX_MASK = (1u << KICKOS_HANDLE_INDEX_BITS) - 1u;
    // Root's thread handle is 0, the threads model's "no thread": its id is this handle instead,
    // whose index no slot holds.
    constexpr __gthread_t ROOT_ID = HANDLE_INDEX_MASK;

    __gthread_t self_id()
    {
        kos_thread_t const h = kos_thread_self();
        if (h == 0u)
        {
            return ROOT_ID;
        }
        return h;
    }

    kos_mutex_t* word_of(__gthread_mutex_t* m)
    {
        return reinterpret_cast<kos_mutex_t*>(m);
    }

    kos_mutex_t* word_of(__gthread_recursive_mutex_t* m)
    {
        return reinterpret_cast<kos_mutex_t*>(&m->__word);
    }

    int errno_of(int rc)
    {
        switch (rc)
        {
            case 0:
            {
                return 0;
            }
            case -KOS_ETIMEDOUT:
            {
                return ETIMEDOUT;
            }
            case -KOS_EBUSY:
            {
                return EBUSY;
            }
            case -KOS_EDEADLK:
            {
                return EDEADLK;
            }
            case -KOS_EPERM:
            {
                return EPERM;
            }
            case -KOS_EAGAIN:
            case -KOS_ENOMEM:
            {
                return EAGAIN;
            }
            case -KOS_EFAULT:
            {
                return EFAULT;
            }
            default:
            {
                return EINVAL;
            }
        }
    }

    // What the dead holder guarded may be torn, and no C++ caller can be told: its task ends.
    [[noreturn]] void owner_died()
    {
        KOS_TRAP_ILLEGAL();
        __builtin_unreachable();
    }

    // Toward the deadline `ns` on the clock `kind` names. A wait cancelled by a kill asks again at
    // the kernel's entry, which ends the thread there.
    int take_until(kos_mutex_t* m, int64_t ns, uint32_t kind)
    {
        int rc = 0;
        if (kind == KOS_LIMIT_REALTIME)
        {
            kos_realtime_t const until = {ns};
            rc = kos_mutex_lock_realtime(m, until);
        }
        else
        {
            kos_monotonic_t const until = {ns};
            rc = kos_mutex_lock(m, until);
        }
        while (rc == -KOS_ECANCELED)
        {
            rc = kickos_mutex_lock_entry(m, kos_limit_word(ns), kind);
        }
        if (rc == -KOS_EOWNERDEAD)
        {
            owner_died();
        }
        return rc;
    }

    int take(kos_mutex_t* m)
    {
        return take_until(m, KOS_NEVER.ns, KOS_LIMIT_MONOTONIC);
    }

    // The absolute `abs` on `clock` as a deadline of the kind that clock's waits take, in
    // nanoseconds since its epoch and saturating: CLOCK_MONOTONIC's timespec is the monotonic
    // clock's own count, so no clock is read. False for a time that is not one or a clock with no
    // wait.
    bool deadline_of(clockid_t clock, __gthread_time_t const* abs, uint32_t* kind, int64_t* ns)
    {
        if (abs->tv_nsec < 0 or abs->tv_nsec >= 1000000000)
        {
            return false;
        }
        if (clock == CLOCK_MONOTONIC)
        {
            *kind = KOS_LIMIT_MONOTONIC;
        }
        else if (clock == CLOCK_REALTIME)
        {
            *kind = KOS_LIMIT_REALTIME;
        }
        else
        {
            return false;
        }
        int64_t const sec = static_cast<int64_t>(abs->tv_sec);
        if (sec < 0)
        {
            *ns = -1;
        }
        else if (sec > (INT64_MAX - 999999999) / 1000000000)
        {
            *ns = INT64_MAX;
        }
        else
        {
            *ns = sec * 1000000000 + static_cast<int64_t>(abs->tv_nsec);
        }
        return true;
    }

    int timed_take(kos_mutex_t* m, clockid_t clock, __gthread_time_t const* abs)
    {
        uint32_t kind = 0;
        int64_t at = 0;
        if (not deadline_of(clock, abs, &kind, &at))
        {
            return EINVAL;
        }
        int const rc = take_until(m, at, kind);
        // A deadline at or before the clock's epoch is a poll, answered -KOS_EBUSY.
        if (rc == -KOS_EBUSY)
        {
            return ETIMEDOUT;
        }
        return errno_of(rc);
    }

    uint32_t holder_of(kos_mutex_t const* m)
    {
        return __atomic_load_n(&m->word, __ATOMIC_RELAXED) & ~KOS_WORD_WAITERS;
    }

    // 0 with the recursive mutex held one level deeper, or the errno of a take that failed.
    // `untimed` bounds a take with no `abs`.
    int recursive_take(__gthread_recursive_mutex_t* m, clockid_t clock,
                       __gthread_time_t const* abs, kos_monotonic_t untimed)
    {
        kos_mutex_t* const w = word_of(m);
        if (holder_of(w) == kickos_self_lock_id())
        {
            m->__depth++;
            return 0;
        }
        int rc = 0;
        if (abs != nullptr)
        {
            rc = timed_take(w, clock, abs);
        }
        else
        {
            rc = errno_of(take_until(w, untimed.ns, KOS_LIMIT_MONOTONIC));
        }
        if (rc == 0)
        {
            m->__depth = 1;
        }
        return rc;
    }

    int cond_answer(int rc)
    {
        if (rc == -KOS_EOWNERDEAD)
        {
            owner_died();
        }
        return errno_of(rc);
    }

    int cond_wait(__gthread_cond_t* c, kos_mutex_t* m)
    {
        return cond_answer(kos_cond_wait(reinterpret_cast<kos_cond_t*>(c), m, KOS_NEVER));
    }

    int cond_timed(__gthread_cond_t* c, __gthread_mutex_t* m, clockid_t clock,
                   __gthread_time_t const* abs)
    {
        uint32_t kind = 0;
        int64_t at = 0;
        if (not deadline_of(clock, abs, &kind, &at))
        {
            return EINVAL;
        }
        kos_cond_t* const cv = reinterpret_cast<kos_cond_t*>(c);
        if (kind == KOS_LIMIT_REALTIME)
        {
            kos_realtime_t const until = {at};
            return cond_answer(kos_cond_wait_realtime(cv, word_of(m), until));
        }
        kos_monotonic_t const until = {at};
        return cond_answer(kos_cond_wait(cv, word_of(m), until));
    }

    void unlink(Run** list, Run* r)
    {
        Run** at = list;
        while (*at != nullptr and *at != r)
        {
            at = &(*at)->next;
        }
        if (*at == r)
        {
            *at = r->next;
        }
    }

    // Off the registry, for the caller alone.
    Run* claim(__gthread_t thread)
    {
        (void)take(&s_runs_lock);
        Run* r = s_runs;
        while (r != nullptr and r->handle != thread)
        {
            r = r->next;
        }
        if (r != nullptr)
        {
            unlink(&s_runs, r);
        }
        (void)kos_mutex_unlock(&s_runs_lock);
        return r;
    }

    void free_list(Run* r)
    {
        while (r != nullptr)
        {
            Run* const next = r->next;
            free(r);
            r = next;
        }
    }

    // Each key's destructor runs on the value the ending thread still holds, again while one
    // leaves a value behind, up to KEY_PASSES times. Out of line: trap_redzone_indirect.txt binds
    // its call by this name.
    __attribute__((noinline)) void run_destructors(KickosThreadWords* own)
    {
        for (int pass = 0; pass < KEY_PASSES; pass++)
        {
            bool ran = false;
            for (uint32_t i = 0; i < KICKOS_THREAD_KEYS; i++)
            {
                void* const value = own->keys[i];
                if (value == nullptr)
                {
                    continue;
                }
                own->keys[i] = nullptr;
                uint32_t const gen = static_cast<uint32_t>(own->key_gens[i]);
                __gthread_key_t const key = i + KICKOS_THREAD_KEYS * gen;
                Key* const k = live(key);
                if (k == nullptr)
                {
                    continue;
                }
                // Read before the key is checked again: a delete and a create of this index in
                // between give the slot another generation.
                void (*const dtor)(void*) = __atomic_load_n(&k->dtor, __ATOMIC_SEQ_CST);
                if (dtor != nullptr and live(key) == k)
                {
                    dtor(value);
                    ran = true;
                }
            }
            if (not ran)
            {
                break;
            }
            // A destructor may have registered a thread_local destructor, as a thread_local
            // first built there does.
            kickos_thread_dtors_run();
        }
    }

    void run_entry(void* arg)
    {
        Run* const r = static_cast<Run*>(arg);
        r->ret = r->func(r->arg);
        if (kickos_user_cas(&r->state, RUN_LIVE, RUN_RETURNED) == RUN_DETACHED)
        {
            (void)take(&s_runs_lock);
            unlink(&s_detached, r);
            (void)kos_mutex_unlock(&s_runs_lock);
            free(r);
        }
        kos_exit(0);
    }
}

extern "C"
{

int __kickos_gthr_mutex_destroy(__gthread_mutex_t*)
{
    return 0;
}

int __kickos_gthr_mutex_lock(__gthread_mutex_t* m)
{
    return errno_of(take(word_of(m)));
}

int __kickos_gthr_mutex_trylock(__gthread_mutex_t* m)
{
    int const rc = take_until(word_of(m), KOS_POLL.ns, KOS_LIMIT_MONOTONIC);
    if (rc == -KOS_EDEADLK)
    {
        return EBUSY;
    }
    return errno_of(rc);
}

int __kickos_gthr_mutex_timedlock(__gthread_mutex_t* m, __gthread_time_t const* abs)
{
    return timed_take(word_of(m), CLOCK_REALTIME, abs);
}

int __kickos_gthr_mutex_clocklock(__gthread_mutex_t* m, clockid_t clock,
                                  __gthread_time_t const* abs)
{
    return timed_take(word_of(m), clock, abs);
}

int __kickos_gthr_mutex_unlock(__gthread_mutex_t* m)
{
    return errno_of(kos_mutex_unlock(word_of(m)));
}

int __kickos_gthr_recursive_mutex_destroy(__gthread_recursive_mutex_t*)
{
    return 0;
}

int __kickos_gthr_recursive_mutex_lock(__gthread_recursive_mutex_t* m)
{
    return recursive_take(m, CLOCK_REALTIME, nullptr, KOS_NEVER);
}

int __kickos_gthr_recursive_mutex_trylock(__gthread_recursive_mutex_t* m)
{
    return recursive_take(m, CLOCK_REALTIME, nullptr, KOS_POLL);
}

int __kickos_gthr_recursive_mutex_timedlock(__gthread_recursive_mutex_t* m,
                                            __gthread_time_t const* abs)
{
    return recursive_take(m, CLOCK_REALTIME, abs, KOS_NEVER);
}

int __kickos_gthr_recursive_mutex_clocklock(__gthread_recursive_mutex_t* m, clockid_t clock,
                                            __gthread_time_t const* abs)
{
    return recursive_take(m, clock, abs, KOS_NEVER);
}

int __kickos_gthr_recursive_mutex_unlock(__gthread_recursive_mutex_t* m)
{
    kos_mutex_t* const w = word_of(m);
    if (holder_of(w) != kickos_self_lock_id())
    {
        return EPERM;
    }
    m->__depth--;
    if (m->__depth != 0)
    {
        return 0;
    }
    return errno_of(kos_mutex_unlock(w));
}

int __kickos_gthr_cond_destroy(__gthread_cond_t*)
{
    return 0;
}

int __kickos_gthr_cond_broadcast(__gthread_cond_t* c)
{
    return errno_of(kos_cond_broadcast(reinterpret_cast<kos_cond_t*>(c)));
}

int __kickos_gthr_cond_signal(__gthread_cond_t* c)
{
    return errno_of(kos_cond_signal(reinterpret_cast<kos_cond_t*>(c)));
}

int __kickos_gthr_cond_wait(__gthread_cond_t* c, __gthread_mutex_t* m)
{
    return cond_wait(c, word_of(m));
}

int __kickos_gthr_cond_timedwait(__gthread_cond_t* c, __gthread_mutex_t* m,
                                 __gthread_time_t const* abs)
{
    return cond_timed(c, m, CLOCK_REALTIME, abs);
}

int __kickos_gthr_cond_clockwait(__gthread_cond_t* c, __gthread_mutex_t* m, clockid_t clock,
                                 __gthread_time_t const* abs)
{
    return cond_timed(c, m, clock, abs);
}

int __kickos_gthr_cond_wait_recursive(__gthread_cond_t* c, __gthread_recursive_mutex_t* m)
{
    uint32_t const depth = m->__depth;
    int const rc = cond_wait(c, word_of(m));
    if (rc != EPERM)
    {
        m->__depth = depth;
    }
    return rc;
}

int __kickos_gthr_key_create(__gthread_key_t* key, void (*dtor)(void*))
{
    for (uint32_t i = 0; i < KICKOS_THREAD_KEYS; i++)
    {
        Key& k = s_keys[i];
        // Claimed first, so no thread sets a value before the generation and destructor are in
        // place.
        if (kickos_user_cas(&k.state, KEY_FREE, KEY_CLAIMED) == KEY_FREE)
        {
            __atomic_store_n(&k.gen, (k.gen + 1u) % KEY_GENERATIONS, __ATOMIC_SEQ_CST);
            __atomic_store_n(&k.dtor, dtor, __ATOMIC_SEQ_CST);
            __atomic_store_n(&k.state, KEY_USED, __ATOMIC_SEQ_CST);
            *key = i + KICKOS_THREAD_KEYS * k.gen;
            return 0;
        }
    }
    return EAGAIN;
}

int __kickos_gthr_key_delete(__gthread_key_t key)
{
    Key* const k = live(key);
    if (k == nullptr or kickos_user_cas(&k->state, KEY_USED, KEY_FREE) != KEY_USED)
    {
        return EINVAL;
    }
    return 0;
}

void* __kickos_gthr_getspecific(__gthread_key_t key)
{
    KickosThreadWords const* const own = kickos_thread_words();
    uint32_t const i = index_of(key);
    if (live(key) == nullptr or own->key_gens[i] != gen_of(key))
    {
        return nullptr;
    }
    return own->keys[i];
}

int __kickos_gthr_setspecific(__gthread_key_t key, void const* value)
{
    if (live(key) == nullptr)
    {
        return EINVAL;
    }
    KickosThreadWords* const own = kickos_thread_words();
    uint32_t const i = index_of(key);
    own->keys[i] = const_cast<void*>(value);
    own->key_gens[i] = gen_of(key);
    return 0;
}

// The thread starts at its creator's priority on the default stack.
int __kickos_gthr_create(__gthread_t* thread, void* (*func)(void*), void* arg)
{
    Run* const r = static_cast<Run*>(malloc(sizeof(Run)));
    if (r == nullptr)
    {
        return EAGAIN;
    }
    r->handle = KOS_THREAD_NONE;
    r->func = func;
    r->arg = arg;
    r->ret = nullptr;
    r->state = RUN_LIVE;
    kos_thread_params p = {};
    p.entry = run_entry;
    p.arg = r;
    p.flags = KOS_THREAD_INHERIT_PRIO;
    kos_thread_t h = KOS_THREAD_NONE;
    int const rc = kos_thread_create(&p, &h);
    if (rc != 0)
    {
        free(r);
        return errno_of(rc);
    }
    (void)take(&s_runs_lock);
    r->handle = h;
    r->next = s_runs;
    s_runs = r;
    // A detached thread whose slot h now holds is gone without having returned.
    Run* gone = nullptr;
    Run** at = &s_detached;
    while (*at != nullptr)
    {
        Run* const d = *at;
        if (((d->handle ^ h) & HANDLE_INDEX_MASK) != 0u)
        {
            at = &d->next;
            continue;
        }
        *at = d->next;
        d->next = gone;
        gone = d;
    }
    (void)kos_mutex_unlock(&s_runs_lock);
    free_list(gone);
    *thread = h;
    return 0;
}

// Any thread of the task joins, and the join returns once the thread is gone by any path, its
// slot free again. A thread that did not return from its function leaves a null value.
int __kickos_gthr_join(__gthread_t thread, void** value)
{
    if (thread == self_id())
    {
        return EDEADLK;
    }
    Run* const r = claim(thread);
    if (r == nullptr)
    {
        return ESRCH;
    }
    int rc = kos_thread_join(r->handle, KOS_NEVER);
    while (rc == -KOS_ECANCELED)
    {
        rc = kos_thread_join(r->handle, KOS_NEVER);
    }
    if (value != nullptr)
    {
        *value = r->ret;
    }
    free(r);
    return 0;
}

// A thread that has returned is freed here, and a running one frees its record on return.
int __kickos_gthr_detach(__gthread_t thread)
{
    (void)take(&s_runs_lock);
    Run* r = s_runs;
    while (r != nullptr and r->handle != thread)
    {
        r = r->next;
    }
    if (r == nullptr)
    {
        (void)kos_mutex_unlock(&s_runs_lock);
        return ESRCH;
    }
    unlink(&s_runs, r);
    bool const returned = kickos_user_cas(&r->state, RUN_LIVE, RUN_DETACHED) == RUN_RETURNED;
    if (not returned)
    {
        r->next = s_detached;
        s_detached = r;
    }
    (void)kos_mutex_unlock(&s_runs_lock);
    if (returned)
    {
        free(r);
    }
    return 0;
}

__gthread_t __kickos_gthr_self(void)
{
    return self_id();
}

int __kickos_gthr_yield(void)
{
    kos_yield();
    return 0;
}

void kickos_keys_thread_end(void)
{
    run_destructors(kickos_thread_words());
}
}
