// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// One struct _reent per thread slot, and the word the kernel stores the running thread's
// into. The user side of kernel/include/kickos/reent.h; compiled only where
// KICKOS_LIBC_REENT is on, and never for the sim, whose libc is the host's.
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

// `extern` IS LOAD-BEARING: a namespace-scope const without it has internal linkage in
// C++, extern "C" changes the language linkage and not that one, and the definition is
// then dropped and the kernel's reference goes unresolved.
#ifdef __XTENSA__
extern void* const kickos_reent_slot = &s_current;
#else
extern void* const kickos_reent_slot = &_impure_ptr;
#endif

void* kickos_reent_acquire(int slot)
{
    if (slot < 0 or slot >= static_cast<int>(KICKOS_THREAD_SLOTS))
    {
        return _GLOBAL_REENT;
    }
    struct _reent* const r = &s_reent[slot];
    // Not reclaimed when a slot is freed: _reclaim_reent would have to run on the death
    // path, and what it reclaims is the per-reent mprec/asctime scratch, allocated only by
    // a thread that used strtod or ctime. A reused slot is re-initialised here instead.
    _REENT_INIT_PTR(r);
    return r;
}
}
