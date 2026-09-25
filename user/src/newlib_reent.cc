// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// One struct _reent per thread slot, where libc finds the running thread's, and the descriptor
// that tells the kernel where both are. The user side of kernel/include/kickos/reent.h; compiled
// on every board but the sim, whose libc is the host's.
//
// Newlib reaches its reentrant state as _REENT, which is __getreent() on lx6 and on the
// conan/newlib builds armv8a and rv64imac link, and _impure_ptr on every other pinned toolchain;
// errno is a member of that state.

#include <kickos/config/system.h> // KICKOS_THREAD_SLOTS, KICKOS_MAX_INSTANCES
#include <kickos/reent.h>

#include <sys/reent.h>

// UNPRIVILEGED APP MEMORY, granted R/W to every unprivileged thread, so a peer can scribble
// another thread's errno: naming, not isolation.
//
// ONE BANK OF KICKOS_THREAD_SLOTS PER INSTANCE: the kernel indexes this array as
// instance * KICKOS_THREAD_SLOTS + slot, and the seam's count below must match this extent
// or a short bank aliases silently onto the process-wide state.
static struct _reent s_reent[KICKOS_MAX_INSTANCES * KICKOS_THREAD_SLOTS];

// Every libc that asks __getreent ships none of its own (esp-elf's is a weak NULL), so this is
// what makes stdio work at all.
#if KICKOS_REENT_IN_TCB
// The first word of the running thread's TLS control block.
extern "C" struct _reent* __getreent(void)
{
    return *static_cast<struct _reent* const*>(__builtin_thread_pointer());
}
#elif KICKOS_REENT_PER_THREAD
// The thread pointer itself, which the kernel resumes every thread with.
extern "C" struct _reent* __getreent(void)
{
    return static_cast<struct _reent*>(__builtin_thread_pointer());
}
#elif defined(__XTENSA__)
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
#if KICKOS_REENT_PER_THREAD
    nullptr,
#elif defined(__XTENSA__)
    &s_current,
#else
    &_impure_ptr,
#endif
    sizeof(struct _reent),
    KICKOS_MAX_INSTANCES * KICKOS_THREAD_SLOTS,
};
}
