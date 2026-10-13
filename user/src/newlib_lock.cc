// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// newlib's retargetable locks. Every static lock and every FILE lock is one recursive lock over
// the kernel mutex seated at KOS_CAP_LIBC_LOCK, so nesting two of them is recursion on one.
// Owner and depth live in the address space they guard. NOT compiled for the sim, where host
// glibc locks its own stdio and malloc.

#include <kickos/sys.h>
#include <kickos/sys/trap.h>

#include <newlib.h>
#include <sys/lock.h>

#if not defined(_RETARGETABLE_LOCKING)
#error "this newlib has no retargetable locking, so stdio and malloc would run unlocked: build with KickOS toolchain 1.1 or later (tools/kickos-toolchain.sh)"
#endif

namespace
{
    // Written only by the thread that holds the kernel mutex, so a thread reads its own handle
    // here only while it is the owner.
    kos_thread_t s_owner = KOS_THREAD_NONE;
    unsigned s_depth = 0;

    // False only for a try that found the mutex held; any other refusal traps. A holder that
    // died inside libc hands the lock over held, its state as it left it.
    bool libc_lock_take(kos_thread_t self, uint32_t timeout_us)
    {
        int const rc = kos_mutex_lock(KOS_CAP_LIBC_LOCK, timeout_us);
        if (rc == -KOS_ETIMEDOUT and timeout_us == 0)
        {
            return false;
        }
        if (rc != 0 and rc != -KOS_EOWNERDEAD)
        {
            KOS_TRAP_ILLEGAL();
        }
        __atomic_store_n(&s_owner, self, __ATOMIC_RELAXED);
        s_depth = 1;
        return true;
    }

    void libc_lock_acquire()
    {
        kos_thread_t const self = kos_thread_self();
        if (__atomic_load_n(&s_owner, __ATOMIC_RELAXED) == self)
        {
            s_depth++;
            return;
        }
        (void)libc_lock_take(self, KOS_TIMEOUT_NONE);
    }

    void libc_lock_release()
    {
        if (__atomic_load_n(&s_owner, __ATOMIC_RELAXED) != kos_thread_self() or s_depth == 0)
        {
            KOS_TRAP_ILLEGAL();
        }
        s_depth--;
        if (s_depth != 0)
        {
            return;
        }
        // Before the unlock: the next owner writes its own handle once it holds the mutex.
        __atomic_store_n(&s_owner, KOS_THREAD_NONE, __ATOMIC_RELAXED);
        if (kos_mutex_unlock(KOS_CAP_LIBC_LOCK) != 0)
        {
            KOS_TRAP_ILLEGAL();
        }
    }

    int libc_lock_try_acquire()
    {
        kos_thread_t const self = kos_thread_self();
        if (__atomic_load_n(&s_owner, __ATOMIC_RELAXED) == self)
        {
            s_depth++;
            return 1;
        }
        if (not libc_lock_take(self, 0))
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

void __retarget_lock_acquire(_LOCK_T)
{
    libc_lock_acquire();
}

void __retarget_lock_acquire_recursive(_LOCK_T)
{
    libc_lock_acquire();
}

int __retarget_lock_try_acquire(_LOCK_T)
{
    return libc_lock_try_acquire();
}

int __retarget_lock_try_acquire_recursive(_LOCK_T)
{
    return libc_lock_try_acquire();
}

void __retarget_lock_release(_LOCK_T)
{
    libc_lock_release();
}

void __retarget_lock_release_recursive(_LOCK_T)
{
    libc_lock_release();
}
}
