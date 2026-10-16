// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Cap-object creator syscalls: sem_create / mutex_create. Each admits the calling task
// against its object ceiling, allocates from a global generational pool and installs the
// owning cap in the creator's table.

#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/sched.h>
#include <kickos/sync.h>

#include <kickos/sys/errno.h>

#include "syscall_internal.h"

namespace kickos
{
    int sem_create(int initial, uint32_t* out_cap)
    {
        IrqLock lock;
        *out_cap = KCAP_INVALID;
        Thread* c = sched::current();
        // The count is an `int` and every post is bounded against KOS_SEM_COUNT_MAX, so
        // the initial value must land in the same range, or a sem is born at INT_MAX and
        // one post is undefined. A negative initial reads as "no token" to sem_wait while
        // every post is swallowed until it climbs back to 0.
        if (initial < 0 or initial > KOS_SEM_COUNT_MAX)
        {
            return -KOS_EINVAL;
        }
        // ASKED BEFORE THE POOL, so the answer does not depend on how full the pool happens
        // to be. -KOS_EAGAIN is unambiguous here: cap_install returns only 0 or
        // -KOS_EMFILE, so the budget is this call's only source of it.
        if (not task_object_admit(CapType::CAP_SEM, c->task))
        {
            return -KOS_EAGAIN;
        }
        int const i = kernel().sems.alloc();
        Semaphore* const s = kernel().sems.at(i); // total over alloc()'s -1
        if (s == nullptr)
        {
            return -KOS_ENOMEM;
        }
        sem_init(s, initial);
        kernel().sem_refs[i] = 1; // this creator's cap is the first reference
        int const obj = kernel().sems.handle_for(i);
        // The pool refusal above and this one are DIFFERENT codes and must stay so: the sem
        // here was allocatable.
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

    // Possession IS the lock/unlock authority (no WAIT/SIGNAL split), so the creator cap
    // carries CAP_TRANSFER only and lock/unlock resolve with need == 0.
    int mutex_create(uint32_t* out_cap)
    {
        IrqLock lock;
        *out_cap = KCAP_INVALID;
        Thread* c = sched::current();
        if (not task_object_admit(CapType::CAP_MUTEX, c->task))
        {
            return -KOS_EAGAIN;
        }
        int const i = kernel().mutexes.alloc();
        Mutex* const m = kernel().mutexes.at(i); // total over alloc()'s -1
        if (m == nullptr)
        {
            return -KOS_ENOMEM;
        }
        mutex_init(m);
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
