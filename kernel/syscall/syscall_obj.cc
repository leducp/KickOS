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
    int sem_create(int initial, uint32_t* out_cap)
    {
        IrqLock lock;
        *out_cap = KCAP_INVALID;
        Thread* c = sched::current();
        if (c == nullptr)
        {
            return -KOS_EPERM; // no caller context (defensive; unreachable from a real syscall)
        }
        // The count is an `int` and every post is bounded against KOS_SEM_COUNT_MAX, so
        // the initial value must land in the same range, or a sem is born at INT_MAX and
        // one post is undefined. A negative initial reads as "no token" to sem_wait while
        // every post is swallowed until it climbs back to 0.
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
        // creator's table; that CAP handle is what userspace sees. A full table is a
        // clean failure: release the just-claimed sem (refs -> 0). The pool refusal above
        // and this one are DIFFERENT codes and must stay so: the sem here was allocatable.
        int const rc = cap_install(c, obj, CapType::CAP_SEM,
                                   CAP_WAIT | CAP_SIGNAL | CAP_TRANSFER, out_cap);
        if (rc != 0)
        {
            kernel().sem_refs[i] = 0;
            kernel().sems.free(obj);
            return rc;
        }
        return 0;
    }

    // --- PI-mutex capability (mirrors sem_create) ------------------------------
    // A mutex lives in the global pool (slotpool.h); a task names it by a per-task
    // CAP_MUTEX capability. Possession IS the lock/unlock authority (no WAIT/SIGNAL
    // split), so the creator cap carries CAP_TRANSFER only and lock/unlock resolve
    // with need == 0. Rollback on a full table mirrors sem_create.
    int mutex_create(uint32_t* out_cap)
    {
        IrqLock lock;
        *out_cap = KCAP_INVALID;
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
        int const rc = cap_install(c, obj, CapType::CAP_MUTEX, CAP_TRANSFER, out_cap);
        if (rc != 0)
        {
            kernel().mutex_refs[i] = 0;
            kernel().mutexes.free(obj);
            return rc;
        }
        return 0;
    }
}
