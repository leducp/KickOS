// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// newlib's retargetable locks: two recursive locks, each a mutex word in the static data of the
// heap's owner, so each address space has its own. malloc's is its own; every other static lock
// and every FILE lock is the libc lock, so nesting two of those is recursion on one. newlib takes
// malloc's inside the libc lock and never the reverse, and a thread taking the libc lock while it
// holds malloc's traps. A holder that died holding either lock left what it guards torn, so the
// lock's next taker traps, or on a chip that holds one task ends the image. NOT compiled for the
// sim, where host glibc locks its own stdio and malloc.

#include <kickos/chip_limits.h> // KICKOS_CHIP_ONE_TASK
#include <kickos/sys.h>
#include <kickos/sys/trap.h>

#include <newlib.h>
#include <sys/lock.h>
#include <unistd.h>

#if not defined(_RETARGETABLE_LOCKING)
#error "this newlib has no retargetable locking, so stdio and malloc would run unlocked: build with KickOS toolchain 1.1 or later (tools/kickos-toolchain.sh)"
#endif

namespace
{
    struct LibcLock
    {
        kos_mutex_t m;
        // Written only by the holder.
        unsigned depth;
    };

    LibcLock s_libc = {KOS_MUTEX_INIT, 0};
    LibcLock s_malloc = {KOS_MUTEX_INIT, 0};

    LibcLock& lock_of(_LOCK_T lock);

    // The holder's lock id: the word names the caller only while the caller holds it.
    uint32_t holder_of(LibcLock const& l)
    {
        return __atomic_load_n(&l.m.word, __ATOMIC_RELAXED) & ~KOS_WORD_WAITERS;
    }

    // On a chip that holds one task the init shares this libc, and the whole image ends.
    [[noreturn]] void libc_lock_owner_died()
    {
#if KICKOS_CHIP_ONE_TASK
        _exit(KOS_EXIT_FAULT);
#else
        KOS_TRAP_ILLEGAL();
        __builtin_unreachable();
#endif
    }

    // False only for a try that found the mutex held; any other refusal traps, a dead owner's
    // included. A wait cancelled by a kill asks again at the kernel's entry, which ends the
    // thread there, where a trap would end its task.
    bool libc_lock_take(LibcLock& l, uint32_t timeout_us)
    {
        int rc = kos_mutex_lock(&l.m, timeout_us);
        while (rc == -KOS_ECANCELED)
        {
            rc = kickos_mutex_lock_entry(&l.m, timeout_us);
        }
        if (rc == -KOS_ETIMEDOUT and timeout_us == 0)
        {
            return false;
        }
        if (rc == -KOS_EOWNERDEAD)
        {
            libc_lock_owner_died();
        }
        if (rc != 0)
        {
            KOS_TRAP_ILLEGAL();
        }
        l.depth = 1;
        return true;
    }

    // The libc lock inside malloc's would invert newlib's order.
    void refuse_inversion(LibcLock const& l, uint32_t self)
    {
        if (&l == &s_libc and holder_of(s_malloc) == self)
        {
            KOS_TRAP_ILLEGAL();
        }
    }

    void libc_lock_acquire(LibcLock& l)
    {
        uint32_t const self = kickos_self_lock_id();
        refuse_inversion(l, self);
        if (holder_of(l) == self)
        {
            l.depth++;
            return;
        }
        (void)libc_lock_take(l, KOS_TIMEOUT_NONE);
    }

    void libc_lock_release(LibcLock& l)
    {
        if (holder_of(l) != kickos_self_lock_id() or l.depth == 0)
        {
            KOS_TRAP_ILLEGAL();
        }
        l.depth--;
        if (l.depth != 0)
        {
            return;
        }
        if (kos_mutex_unlock(&l.m) != 0)
        {
            KOS_TRAP_ILLEGAL();
        }
    }

    int libc_lock_try_acquire(LibcLock& l)
    {
        uint32_t const self = kickos_self_lock_id();
        refuse_inversion(l, self);
        if (holder_of(l) == self)
        {
            l.depth++;
            return 1;
        }
        if (not libc_lock_take(l, 0))
        {
            return 0;
        }
        return 1;
    }
}

extern "C"
{

// Defined all together: libc carries a no-op default of each, and one missing here links that
// object, whose duplicates then collide.
struct __lock
{
    char unused;
};

struct __lock __lock___sfp_recursive_mutex;
struct __lock __lock___atexit_recursive_mutex;
struct __lock __lock___at_quick_exit_mutex;
struct __lock __lock___malloc_recursive_mutex;
struct __lock __lock___env_recursive_mutex;
struct __lock __lock___tz_mutex;
struct __lock __lock___dd_hash_mutex;
struct __lock __lock___arc4random_mutex;

void __retarget_lock_init(_LOCK_T*)
{
}

void __retarget_lock_init_recursive(_LOCK_T*)
{
}

void __retarget_lock_close(_LOCK_T)
{
}

void __retarget_lock_close_recursive(_LOCK_T)
{
}

void __retarget_lock_acquire(_LOCK_T lock)
{
    libc_lock_acquire(lock_of(lock));
}

void __retarget_lock_acquire_recursive(_LOCK_T lock)
{
    libc_lock_acquire(lock_of(lock));
}

int __retarget_lock_try_acquire(_LOCK_T lock)
{
    return libc_lock_try_acquire(lock_of(lock));
}

int __retarget_lock_try_acquire_recursive(_LOCK_T lock)
{
    return libc_lock_try_acquire(lock_of(lock));
}

void __retarget_lock_release(_LOCK_T lock)
{
    libc_lock_release(lock_of(lock));
}

void __retarget_lock_release_recursive(_LOCK_T lock)
{
    libc_lock_release(lock_of(lock));
}

// The heap seat (newlib_sbrk.cc), which an image that allocates links; this no-op stands in where
// none does.
__attribute__((weak)) void kickos_heap_seat(void)
{
}

// libc's own, with the heap seated at the first take: before the image's first allocation, from
// whichever path makes it. libc's copy would collide with these.
void __malloc_lock(struct _reent*)
{
    __retarget_lock_acquire_recursive(&__lock___malloc_recursive_mutex);
    kickos_heap_seat();
}

void __malloc_unlock(struct _reent*)
{
    __retarget_lock_release_recursive(&__lock___malloc_recursive_mutex);
}
}

namespace
{
    // By identity, so a FILE whose lock word init never wrote still lands on the libc lock.
    LibcLock& lock_of(_LOCK_T lock)
    {
        if (lock == &__lock___malloc_recursive_mutex)
        {
            return s_malloc;
        }
        return s_libc;
    }
}

kos_mutex_t* kos_libc_lock(enum kos_libc_lock which)
{
    if (which == KOS_LIBC_LOCK_MALLOC)
    {
        return &s_malloc.m;
    }
    return &s_libc.m;
}
