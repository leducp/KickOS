// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The priority-inheritance mutex object, apart from its operations (sync.h), so that an object
// embedding one need not include the scheduler.

#ifndef KICKOS_MUTEX_H
#define KICKOS_MUTEX_H

#include <kickos/list.h>

namespace kickos
{
    struct Thread;

    // Priority-inheritance mutex (CAP_MUTEX). `owner != nullptr` is the whole lock state. An
    // owner-died wake reaches the waiter only through its Thread::wait_result.
    struct Mutex
    {
        Thread* owner = nullptr;
        List waiters;
        Mutex* next_held = nullptr; // intrusive link in the owner's held list
        // Its holder died and no waiter took it over: the next take answers -KOS_EOWNERDEAD.
        bool owner_died = false;
    };

    // A heap owner's C library locks, one per pseudo-handle <kickos/sys/cap_index.h> names.
    enum LibcLockIndex : int
    {
        LIBC_LOCK_STDIO,
        LIBC_LOCK_MALLOC,
        LIBC_LOCK_COUNT
    };
}

#endif
