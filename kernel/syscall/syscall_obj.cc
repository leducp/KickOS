// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Cap-object creator syscalls: sem_create / mutex_create. Each allocates from a
// global generational pool and installs the owning cap in the creator's table.
// Split out of syscall.cc; the endpoint creator lives with the rest of the IPC
// path in syscall_ipc.cc. External linkage; declared in syscall_internal.h.

#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/sched.h>
#include <kickos/sync.h>

#include <kickos/sys/errno.h>

#include "syscall_internal.h"

namespace kickos
{
    // --- Semaphore capabilities (per-task cap table over the global pool) -------
    // A sem lives in the global generational pool (slotpool.h); a task names it by
    // a per-task CAP_SEM capability (cap.h). cap_resolve is the single validate-and-
    // resolve chokepoint (per-task cap-gen guard, then the pool's object-gen guard).
    // sem_wait needs CAP_WAIT, sem_post needs CAP_SIGNAL.
    int sem_create(int initial)
    {
        IrqLock lock;
        Thread* c = sched::current();
        if (c == nullptr)
        {
            return -KOS_EPERM; // no caller context (defensive; unreachable from a real syscall)
        }
        // The count is an `int` and every later post is bounded against
        // KOS_SEM_COUNT_MAX, so the initial value has to land inside the same range --
        // otherwise a sem could be BORN at INT_MAX and one post would be UB. A negative
        // initial is refused too: sem_wait reads it as "no token" while every post is
        // swallowed until it climbs back to 0, which no caller means to ask for.
        if (initial < 0 or initial > KOS_SEM_COUNT_MAX)
        {
            return -KOS_EINVAL; // initial count outside [0, KOS_SEM_COUNT_MAX]
        }
        int const i = kernel().sems.alloc();
        if (i < 0)
        {
            return -KOS_ENOMEM; // sem pool exhausted
        }
        sem_init(kernel().sems.at(i), initial);
        kernel().sem_refs[i] = 1; // this creator's cap is the first reference
        int const obj = kernel().sems.handle_for(i);
        // Install the owning cap with full rights (WAIT|SIGNAL|TRANSFER) in the
        // creator's table; the returned CAP handle is what userspace sees. A full
        // table is a clean failure: release the just-claimed sem (refs -> 0).
        int const cap = cap_install(c, obj, CapType::CAP_SEM,
                                    CAP_WAIT | CAP_SIGNAL | CAP_TRANSFER);
        if (cap < 0)
        {
            kernel().sem_refs[i] = 0;
            kernel().sems.free(obj);
            return -KOS_ENOMEM; // cap table full
        }
        return cap;
    }

    // --- PI-mutex capability (mirrors sem_create) ------------------------------
    // A mutex lives in the global pool (slotpool.h); a task names it by a per-task
    // CAP_MUTEX capability. Possession IS the lock/unlock authority (no WAIT/SIGNAL
    // split), so the creator cap carries CAP_TRANSFER only and lock/unlock resolve
    // with need == 0. Rollback on a full table mirrors sem_create.
    int mutex_create()
    {
        IrqLock lock;
        Thread* c = sched::current();
        if (c == nullptr)
        {
            return -KOS_EPERM; // no caller context (defensive)
        }
        int const i = kernel().mutexes.alloc();
        if (i < 0)
        {
            return -KOS_ENOMEM; // mutex pool exhausted
        }
        mutex_init(kernel().mutexes.at(i));
        kernel().mutex_refs[i] = 1;
        int const obj = kernel().mutexes.handle_for(i);
        int const cap = cap_install(c, obj, CapType::CAP_MUTEX, CAP_TRANSFER);
        if (cap < 0)
        {
            kernel().mutex_refs[i] = 0;
            kernel().mutexes.free(obj);
            return -KOS_ENOMEM; // cap table full
        }
        return cap;
    }
}
