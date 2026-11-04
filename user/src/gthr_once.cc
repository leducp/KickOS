// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The threads model's once (gthr-kickos.h), on the once's word: untouched, running, done. A
// function that throws leaves it untouched for the next caller, so this TU is built with
// exceptions. A waiter parks on the plain word and lends the running thread no priority.

#include <kickos/reent.h>
#include <kickos/sys.h>
#include <kickos/sys/user_cas.h>

#include <bits/gthr.h>

namespace
{
    constexpr uint32_t UNTOUCHED = 0;
    constexpr uint32_t RUNNING = 1;
    constexpr uint32_t DONE = 2;

    // A once the thread runs, on its stack, listed from its cell.
    struct Running
    {
        __gthread_once_t const* once;
        Running* next;
    };

    bool runs(KickosThreadWords const* own, __gthread_once_t const* once)
    {
        Running const* r = static_cast<Running const*>(own->once);
        while (r != nullptr)
        {
            if (r->once == once)
            {
                return true;
            }
            r = r->next;
        }
        return false;
    }

    // The value, keeping the kernel's waiters bit, and every waiter woken.
    void settle(uint32_t* word, uint32_t value)
    {
        uint32_t v = __atomic_load_n(word, __ATOMIC_RELAXED);
        while (true)
        {
            uint32_t const seen = kickos_user_cas(word, v, value | (v & KOS_WORD_WAITERS));
            if (seen == v)
            {
                break;
            }
            v = seen;
        }
        if ((v & KOS_WORD_WAITERS) != 0u)
        {
            (void)kos_word_wake(word, 0xFFFFFFFFu, 0u);
        }
    }

    struct Unwind
    {
        KickosThreadWords* own;
        Running const* node;
        uint32_t* word;
        bool done;

        ~Unwind()
        {
            own->once = node->next;
            if (not done)
            {
                settle(word, UNTOUCHED);
            }
        }
    };

    // Where a frame registers its unwind context on entry (SjLj), that registration's first use
    // runs the unwinder's own once, which finds this thread running it and runs it inline: the
    // function may then be done already.
    __attribute__((noinline)) void run(KickosThreadWords* own, Running const* node, uint32_t* word,
                                       void (*func)(void))
    {
        Unwind u{own, node, word, false};
        if ((__atomic_load_n(word, __ATOMIC_ACQUIRE) & ~KOS_WORD_WAITERS) != DONE)
        {
            func();
        }
        u.done = true;
        settle(word, DONE);
    }
}

extern "C" int __kickos_gthr_once(__gthread_once_t* once, void (*func)(void))
{
    uint32_t* const word = &once->__state;
    uint32_t v = __atomic_load_n(word, __ATOMIC_ACQUIRE);
    if ((v & ~KOS_WORD_WAITERS) == DONE)
    {
        return 0;
    }
    KickosThreadWords* const own = kickos_thread_words();
    if (runs(own, once))
    {
        func();
        settle(word, DONE);
        return 0;
    }
    while (true)
    {
        uint32_t const state = v & ~KOS_WORD_WAITERS;
        if (state == DONE)
        {
            return 0;
        }
        if (state == UNTOUCHED)
        {
            uint32_t const seen = kickos_user_cas(word, v, RUNNING | (v & KOS_WORD_WAITERS));
            if (seen == v)
            {
                Running node{once, static_cast<Running*>(own->once)};
                own->once = &node;
                run(own, &node, word, func);
                return 0;
            }
            v = seen;
            continue;
        }
        uint32_t const want = v | KOS_WORD_WAITERS;
        if (v != want)
        {
            uint32_t const seen = kickos_user_cas(word, v, want);
            if (seen != v)
            {
                v = seen;
                continue;
            }
        }
        (void)kos_word_wait(word, want, KOS_TIMEOUT_NONE);
        v = __atomic_load_n(word, __ATOMIC_ACQUIRE);
    }
}
