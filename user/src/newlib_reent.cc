// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// One struct _reent per thread slot, where libc finds the running thread's, the descriptor
// that tells the kernel where both are, and the exit-time release of the scratch a _REENT_SMALL
// libc allocates into a slot. The user side of kernel/include/kickos/reent.h; compiled on every
// board but the sim, whose libc is the host's.
//
// Newlib reaches its reentrant state as _REENT: __getreent() on lx6, armv8a and rv64imac,
// and _impure_ptr on the single-core Cortex-M, RV32 and RX targets. Every cross target links
// its own pinned conan/newlib build; errno is a member of that state.

#include <kickos/config/system.h> // KICKOS_THREAD_SLOTS, KICKOS_MAX_INSTANCES
#include <kickos/reent.h>

#include <malloc.h>
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

#ifdef _REENT_SMALL
// Not _reclaim_reent: it skips the state _impure_ptr names, which on the static-reent targets
// is the caller's own, and it runs the stdio cleanup hook, which closes the FILEs every thread
// shares. The signal table stays for the reason _reclaim_reent gives.
void kickos_reent_release(void)
{
    struct _reent* const r = _REENT;
    struct _mprec* const mp = r->_mp;
    if (mp != nullptr)
    {
        if (mp->_freelist != nullptr)
        {
            // Balloc sizes the list _Kmax + 1.
            for (size_t k = 0; k <= _Kmax; k++)
            {
                struct _Bigint* b = mp->_freelist[k];
                while (b != nullptr)
                {
                    struct _Bigint* const next = b->_next;
                    _free_r(r, b);
                    b = next;
                }
            }
            _free_r(r, mp->_freelist);
        }
        _free_r(r, mp->_result);
        struct _Bigint* p5 = mp->_p5s;
        while (p5 != nullptr)
        {
            struct _Bigint* const next = p5->_next;
            _free_r(r, p5);
            p5 = next;
        }
        _free_r(r, mp);
        r->_mp = nullptr;
    }
    _free_r(r, r->_emergency);
    r->_emergency = nullptr;
    _free_r(r, r->_r48);
    r->_r48 = nullptr;
    _free_r(r, r->_localtime_buf);
    r->_localtime_buf = nullptr;
    _free_r(r, r->_asctime_buf);
    r->_asctime_buf = nullptr;
    _free_r(r, r->_misc);
    r->_misc = nullptr;
    _free_r(r, r->_signal_buf);
    r->_signal_buf = nullptr;
    _free_r(r, r->_cvtbuf);
    r->_cvtbuf = nullptr;
    r->_cvtlen = 0;
}
#endif
}
