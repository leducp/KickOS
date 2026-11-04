// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Replaces libsupc++'s atexit_thread.o: each thread keeps its thread_local destructors on a list
// headed in its cell, which its own exit runs. A program with no thread_local destructor never
// extracts this member, so the weak no-op in newlib_reent.cc stands in.

#include <kickos/reent.h>

#include <malloc.h>
#include <sys/reent.h>

namespace
{
    struct ThreadDtor
    {
        void (*dtor)(void*);
        void* obj;
        ThreadDtor* next;
    };
}

extern "C" int __cxa_thread_atexit(void (*dtor)(void*), void* obj, void*) noexcept
{
    ThreadDtor* const d = static_cast<ThreadDtor*>(_malloc_r(_REENT, sizeof(ThreadDtor)));
    if (d == nullptr)
    {
        return -1;
    }
    KickosThreadWords* const own = kickos_thread_words();
    d->dtor = dtor;
    d->obj = obj;
    d->next = static_cast<ThreadDtor*>(own->thread_dtors);
    own->thread_dtors = d;
    return 0;
}

namespace
{
    // Last registered first. A destructor may register another, which then runs too. Out of
    // line: trap_redzone_indirect.txt binds its call by this name.
    __attribute__((noinline)) void run_list(KickosThreadWords* own)
    {
        while (own->thread_dtors != nullptr)
        {
            ThreadDtor* const d = static_cast<ThreadDtor*>(own->thread_dtors);
            own->thread_dtors = d->next;
            d->dtor(d->obj);
            _free_r(_REENT, d);
        }
    }
}

extern "C" void kickos_thread_dtors_run(void)
{
    run_list(kickos_thread_words());
}
