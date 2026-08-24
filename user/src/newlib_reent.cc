// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// One struct _reent per thread slot, and the word the kernel seats the running thread's
// into. The user side of kernel/include/kickos/reent.h; compiled only where
// every board but the sim, whose libc is the host's.
//
// Newlib reaches its reentrant state as _REENT, which expands to _impure_ptr on every
// pinned toolchain but the Xtensa one, and calls __errno() nowhere. Swapping that one
// pointer is therefore what moves errno AND every other member of the state.

#include <kickos/config/system.h> // KICKOS_THREAD_SLOTS
#include <kickos/reent.h>

#include <string.h> // memset, reached by _REENT_INIT_PTR
#include <sys/reent.h>

// UNPRIVILEGED APP MEMORY. This lands in .appbss, the window granted R/W to every
// unprivileged thread, so a peer can scribble another thread's errno. That is the same
// posture thread_local storage has: naming, not isolation.
static struct _reent s_reent[KICKOS_THREAD_SLOTS];

#ifdef __XTENSA__
// esp-elf newlib resolves _REENT through __getreent() and ships a weak fallback returning
// NULL, so this override is what makes stdio work at all on that arch; newlib_stubs.cc
// owns it when this file is not compiled.
static struct _reent* s_current = _GLOBAL_REENT;

extern "C" struct _reent* __getreent(void)
{
    return s_current;
}
#endif

extern "C"
{

void* kickos_reent_acquire(int slot)
{
    if (slot < 0 or slot >= static_cast<int>(KICKOS_THREAD_SLOTS))
    {
        return _GLOBAL_REENT;
    }
    return &s_reent[slot];
}

void kickos_reent_init(void* reent)
{
    struct _reent* const r = static_cast<struct _reent*>(reent);
    if (r == _GLOBAL_REENT)
    {
        return;
    }
    // A REUSED SLOT LEAKS THE PRIOR OCCUPANT'S SCRATCH. _REENT_INIT_PTR overwrites the
    // mprec and asctime pointers a strtod or ctime caller allocated, and _reclaim_reent,
    // which would return them, cannot be called here: it closes stdio, and every thread's
    // _stdin/_stdout/_stderr point at the one process-wide __sf[3].
    _REENT_INIT_PTR(r);
}

void kickos_reent_seat(void* reent)
{
    // The store the kernel is not allowed to make itself: struct _reent* is the object's
    // real type, and it is in scope only here.
#ifdef __XTENSA__
    s_current = static_cast<struct _reent*>(reent);
#else
    _impure_ptr = static_cast<struct _reent*>(reent);
#endif
}
}
