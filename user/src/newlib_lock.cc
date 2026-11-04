// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// newlib's retargetable locks, each a recursive lock of its own: the mutex word and the depth
// its holder writes, in a struct __lock in the static data of the heap's owner, so each address
// space has its own. A static lock is a named object; a FILE holds its own, unheld from the
// FILE's making and never initialised by libc. newlib takes malloc's lock inside the others and
// never the reverse, and a thread taking any other lock while it holds malloc's traps. A holder
// that died holding a lock left what it guards torn, so the lock's next taker traps. NOT compiled
// for the sim, where host glibc locks its own stdio and malloc.

#include <kickos/sys.h>
#include <kickos/sys/trap.h>

#include <newlib.h>
#include <sys/lock.h>

#if not defined(_RETARGETABLE_LOCKING)
#error "this newlib has no retargetable locking, so stdio and malloc would run unlocked: build with KickOS toolchain 1.2 or later (tools/kickos-toolchain.sh)"
#endif

// The stream locks need newlib's stdio, which the lock layer's unit test builds without.
#if defined(_NEWLIB_VERSION)
#include <stdio.h>
#endif

extern "C"
{
// Defined all together: libc carries a no-op default of each, and one missing here links that
// object, whose duplicates then collide.
struct __lock __lock___sfp_recursive_mutex;
struct __lock __lock___atexit_recursive_mutex;
struct __lock __lock___at_quick_exit_mutex;
struct __lock __lock___malloc_recursive_mutex;
struct __lock __lock___env_recursive_mutex;
struct __lock __lock___tz_mutex;
struct __lock __lock___dd_hash_mutex;
struct __lock __lock___arc4random_mutex;
}

namespace
{
    kos_mutex_t* word_of(_LOCK_T lock)
    {
        return reinterpret_cast<kos_mutex_t*>(&lock->__word);
    }

    // The holder's lock id: the word names the caller only while the caller holds it.
    uint32_t holder_of(_LOCK_T lock)
    {
        return __atomic_load_n(&lock->__word, __ATOMIC_RELAXED) & ~KOS_WORD_WAITERS;
    }

    [[noreturn]] void libc_lock_owner_died()
    {
        KOS_TRAP_ILLEGAL();
        __builtin_unreachable();
    }

    // False only for a try that found the mutex held; any other refusal traps, a dead owner's
    // included. A wait cancelled by a kill asks again at the kernel's entry, which ends the
    // thread there, where a trap would end its task.
    bool libc_lock_take(_LOCK_T lock, uint32_t timeout_us)
    {
        int rc = kos_mutex_lock(word_of(lock), timeout_us);
        while (rc == -KOS_ECANCELED)
        {
            rc = kickos_mutex_lock_entry(word_of(lock), timeout_us);
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
        lock->__depth = 1;
        return true;
    }

    // Any other lock inside malloc's would invert newlib's order.
    void refuse_inversion(_LOCK_T lock, uint32_t self)
    {
        if (lock != &__lock___malloc_recursive_mutex
            and holder_of(&__lock___malloc_recursive_mutex) == self)
        {
            KOS_TRAP_ILLEGAL();
        }
    }

    void libc_lock_acquire(_LOCK_T lock)
    {
        uint32_t const self = kickos_self_lock_id();
        refuse_inversion(lock, self);
        if (holder_of(lock) == self)
        {
            lock->__depth++;
            return;
        }
        (void)libc_lock_take(lock, KOS_TIMEOUT_NONE);
    }

    void libc_lock_release(_LOCK_T lock)
    {
        if (holder_of(lock) != kickos_self_lock_id() or lock->__depth == 0)
        {
            KOS_TRAP_ILLEGAL();
        }
        lock->__depth--;
        if (lock->__depth != 0)
        {
            return;
        }
        if (kos_mutex_unlock(word_of(lock)) != 0)
        {
            KOS_TRAP_ILLEGAL();
        }
    }

    int libc_lock_try_acquire(_LOCK_T lock)
    {
        uint32_t const self = kickos_self_lock_id();
        refuse_inversion(lock, self);
        if (holder_of(lock) == self)
        {
            lock->__depth++;
            return 1;
        }
        if (not libc_lock_take(lock, 0))
        {
            return 0;
        }
        return 1;
    }
}

extern "C"
{

// Every lock newlib takes is a named object or in its FILE, so a lock to make or retire is a
// newlib this layer does not serve.
void __retarget_lock_init(_LOCK_T*)
{
    KOS_TRAP_ILLEGAL();
}

void __retarget_lock_init_recursive(_LOCK_T*)
{
    KOS_TRAP_ILLEGAL();
}

void __retarget_lock_close(_LOCK_T)
{
    KOS_TRAP_ILLEGAL();
}

void __retarget_lock_close_recursive(_LOCK_T)
{
    KOS_TRAP_ILLEGAL();
}

void __retarget_lock_acquire(_LOCK_T lock)
{
    libc_lock_acquire(lock);
}

void __retarget_lock_acquire_recursive(_LOCK_T lock)
{
    libc_lock_acquire(lock);
}

int __retarget_lock_try_acquire(_LOCK_T lock)
{
    return libc_lock_try_acquire(lock);
}

int __retarget_lock_try_acquire_recursive(_LOCK_T lock)
{
    return libc_lock_try_acquire(lock);
}

void __retarget_lock_release(_LOCK_T lock)
{
    libc_lock_release(lock);
}

void __retarget_lock_release_recursive(_LOCK_T lock)
{
    libc_lock_release(lock);
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

kos_mutex_t* kos_libc_malloc_lock(void)
{
    return word_of(&__lock___malloc_recursive_mutex);
}

#if defined(_NEWLIB_VERSION)
kos_mutex_t* kos_libc_file_lock(struct __sFILE* stream)
{
    return word_of(&stream->_lock);
}

// POSIX's own hold of a stream, which newlib declares and leaves to the system.
extern "C"
{
void flockfile(FILE* stream)
{
    libc_lock_acquire(&stream->_lock);
}

int ftrylockfile(FILE* stream)
{
    if (libc_lock_try_acquire(&stream->_lock) == 0)
    {
        return -1;
    }
    return 0;
}

void funlockfile(FILE* stream)
{
    libc_lock_release(&stream->_lock);
}
}
#endif
