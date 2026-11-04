// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The layouts the toolchain freezes, checked against the KickOS types the user runtime implements
// them with: the gthreads types of its threads model and newlib's lock. A toolchain that
// disagrees fails this build.

#include <stddef.h>
#include <stdint.h>

#include <kickos/sys.h>

#include <bits/gthr.h>
#include <sys/lock.h>
#include <sys/reent.h>

#if not defined(_GLIBCXX_GCC_GTHR_KICKOS_H)
#error "this toolchain has no KickOS threads model: build with KickOS toolchain 1.2 or later (tools/kickos-toolchain.sh)"
#endif

static_assert(sizeof(__gthread_mutex_t) == sizeof(kos_mutex_t), "gthread mutex size");
static_assert(alignof(__gthread_mutex_t) == alignof(kos_mutex_t), "gthread mutex alignment");
static_assert(offsetof(__gthread_mutex_t, __word) == offsetof(kos_mutex_t, word),
              "gthread mutex word");

static_assert(offsetof(__gthread_recursive_mutex_t, __word) == offsetof(kos_mutex_t, word),
              "gthread recursive mutex word");
static_assert(offsetof(__gthread_recursive_mutex_t, __depth) == sizeof(kos_mutex_t),
              "gthread recursive mutex depth");
static_assert(sizeof(__gthread_recursive_mutex_t::__depth) == sizeof(uint32_t),
              "gthread recursive mutex depth size");
static_assert(sizeof(__gthread_recursive_mutex_t) == sizeof(kos_mutex_t) + sizeof(uint32_t),
              "gthread recursive mutex size");

// A plain word kos_word_wait and kos_word_wake take.
static_assert(sizeof(__gthread_cond_t) == sizeof(uint32_t), "gthread cond size");
static_assert(alignof(__gthread_cond_t) == alignof(uint32_t), "gthread cond alignment");
static_assert(sizeof(__gthread_once_t) == sizeof(uint32_t), "gthread once size");
static_assert(alignof(__gthread_once_t) == alignof(uint32_t), "gthread once alignment");

static_assert(sizeof(__gthread_key_t) == sizeof(uint32_t), "gthread key size");
static_assert(sizeof(__gthread_t) == sizeof(kos_thread_t), "gthread thread size");

namespace
{
    constexpr kos_mutex_t s_kos_mutex = KOS_MUTEX_INIT;
    constexpr __gthread_mutex_t s_mutex = __GTHREAD_MUTEX_INIT;
    constexpr __gthread_recursive_mutex_t s_recursive = __GTHREAD_RECURSIVE_MUTEX_INIT;
    constexpr __gthread_cond_t s_cond = __GTHREAD_COND_INIT;
    constexpr __gthread_once_t s_once = __GTHREAD_ONCE_INIT;
}

static_assert(s_mutex.__word == s_kos_mutex.word, "gthread mutex initialiser");
static_assert(s_recursive.__word == s_kos_mutex.word and s_recursive.__depth == 0,
              "gthread recursive mutex initialiser");
static_assert(s_cond.__seq == 0 and s_once.__state == 0, "gthread cond and once initialisers");

// newlib's lock, a named static object or embedded in a FILE: the mutex word, then the depth.
static_assert(offsetof(struct __lock, __word) == offsetof(kos_mutex_t, word), "newlib lock word");
static_assert(offsetof(struct __lock, __depth) == sizeof(kos_mutex_t), "newlib lock depth");
static_assert(sizeof(__lock::__depth) == sizeof(uint32_t), "newlib lock depth size");
static_assert(sizeof(struct __lock) == sizeof(kos_mutex_t) + sizeof(uint32_t),
              "newlib lock size");
static_assert(alignof(struct __lock) == alignof(kos_mutex_t), "newlib lock alignment");
static_assert(__is_same(_flock_t, struct __lock), "a FILE holds its own newlib lock");
