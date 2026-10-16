// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// newlib's retargetable locks: two recursive locks, each over a kernel mutex of the heap's owner.
// malloc's is its own; every other static lock and every FILE lock is the libc lock, so nesting
// two of those is recursion on one. newlib takes malloc's inside the libc lock and never the
// reverse, and a thread taking the libc lock while it holds malloc's traps. A holder that died
// inside malloc leaves the heap torn, so its lock's next taker traps; a holder that died inside
// stdio hands the libc lock over held. Owner and depth live in the address space they guard. NOT
// compiled for the sim, where host glibc locks its own stdio and malloc.

#include <kickos/sys.h>
#include <kickos/sys/trap.h>

#include <newlib.h>
#include <sys/lock.h>

#if not defined(_RETARGETABLE_LOCKING)
#error "this newlib has no retargetable locking, so stdio and malloc would run unlocked: build with KickOS toolchain 1.1 or later (tools/kickos-toolchain.sh)"
#endif

namespace
{
    struct LibcLock
    {
        kos_cap_t mutex;
        // Written only by the thread that holds the kernel mutex, or the taker of a dead owner's
        // lock, so a thread reads its own handle here only while it is the owner.
        kos_thread_t owner;
        unsigned depth;
    };

    LibcLock s_libc = {KOS_CAP_LIBC_LOCK, KOS_THREAD_NONE, 0};
    LibcLock s_malloc = {KOS_CAP_MALLOC_LOCK, KOS_THREAD_NONE, 0};

    LibcLock& lock_of(_LOCK_T lock);

    // False only for a try that found the mutex held; any other refusal traps, and so does a
    // dead owner of malloc's lock. A wait cancelled by a kill asks again: the kernel ends the
    // thread at that entry, where a trap would end its task.
    bool libc_lock_take(LibcLock& l, kos_thread_t self, uint32_t timeout_us)
    {
        int rc = kos_mutex_lock(l.mutex, timeout_us);
        while (rc == -KOS_ECANCELED)
        {
            rc = kos_mutex_lock(l.mutex, timeout_us);
        }
        if (rc == -KOS_ETIMEDOUT and timeout_us == 0)
        {
            return false;
        }
        if (rc == -KOS_EOWNERDEAD and &l == &s_malloc)
        {
            KOS_TRAP_ILLEGAL();
        }
        if (rc != 0 and rc != -KOS_EOWNERDEAD)
        {
            KOS_TRAP_ILLEGAL();
        }
        __atomic_store_n(&l.owner, self, __ATOMIC_RELAXED);
        l.depth = 1;
        return true;
    }

    // The libc lock inside malloc's would invert newlib's order.
    void refuse_inversion(LibcLock const& l, kos_thread_t self)
    {
        if (&l == &s_libc and __atomic_load_n(&s_malloc.owner, __ATOMIC_RELAXED) == self)
        {
            KOS_TRAP_ILLEGAL();
        }
    }

    void libc_lock_acquire(LibcLock& l)
    {
        kos_thread_t const self = kos_thread_self();
        refuse_inversion(l, self);
        if (__atomic_load_n(&l.owner, __ATOMIC_RELAXED) == self)
        {
            l.depth++;
            return;
        }
        (void)libc_lock_take(l, self, KOS_TIMEOUT_NONE);
    }

    void libc_lock_release(LibcLock& l)
    {
        if (__atomic_load_n(&l.owner, __ATOMIC_RELAXED) != kos_thread_self() or l.depth == 0)
        {
            KOS_TRAP_ILLEGAL();
        }
        l.depth--;
        if (l.depth != 0)
        {
            return;
        }
        // Before the unlock: the next owner writes its own handle once it holds the mutex.
        __atomic_store_n(&l.owner, KOS_THREAD_NONE, __ATOMIC_RELAXED);
        if (kos_mutex_unlock(l.mutex) != 0)
        {
            KOS_TRAP_ILLEGAL();
        }
    }

    int libc_lock_try_acquire(LibcLock& l)
    {
        kos_thread_t const self = kos_thread_self();
        refuse_inversion(l, self);
        if (__atomic_load_n(&l.owner, __ATOMIC_RELAXED) == self)
        {
            l.depth++;
            return 1;
        }
        if (not libc_lock_take(l, self, 0))
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
