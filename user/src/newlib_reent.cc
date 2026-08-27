// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// One struct _reent per thread slot, the word the kernel seats the running thread's into,
// and the descriptor that tells the kernel where both are. The user side of
// kernel/include/kickos/reent.h; compiled on every board but the sim, whose libc is the
// host's.
//
// Newlib reaches its reentrant state as _REENT, which expands to _impure_ptr on every
// pinned toolchain but the Xtensa one, and calls __errno() nowhere. Swapping that one
// pointer is therefore what moves errno AND every other member of the state.

#include <kickos/config/system.h> // KICKOS_THREAD_SLOTS
#include <kickos/reent.h>

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

// NO CAST IN ANY INITIALISER. Every member takes the implicit conversion to void*, so this
// is statically initialised; a cast would sink it into a ctor, and this file's ctors run
// from root_entry, long after the kernel has read the descriptor.
KickosReentSeam const kickos_reent_seam = {
    s_reent,
    _GLOBAL_REENT,
#ifdef __XTENSA__
    &s_current,
#else
    &_impure_ptr,
#endif
    sizeof(struct _reent),
    KICKOS_THREAD_SLOTS,
};
}
