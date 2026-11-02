// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// libcrace's peer, a program of its own, which main creates with a role: the arm that needs a
// task of its own runs here, its entry thread the driver of threads it spawns, and its task's
// exit status is the answer. The task ending by KOS_EXIT_FAULT is the other answer.

#include <string.h>

#include <kickos/kos.h>
#include <kickos/sys.h>

#include "libcrace.h"

namespace
{
    using namespace libcrace;

    constexpr int REFUSED = 1;

#if defined(_NEWLIB_VERSION)
    // A holder of the malloc lock is killed while it runs, a taker parks behind it, and the
    // holder releases. 0 when the taker took the lock and the task lived.
    int running_killed_holder_releases()
    {
        uint32_t core_mask = 0;
        if (not pin_main(&core_mask))
        {
            return REFUSED;
        }
        auto setter = spawn_in(release_setter, nullptr, "mgo", WORKER_PRIO, core_mask);
        // Below the entry, so it holds the lock and spins only while the entry waits.
        auto holder = spawn_in(running_holder, nullptr, "mrunner", KICKOS_PRIO_MIN, core_mask);
        bool ran = setter.valid() and holder.valid()
                   and kos_sem_wait(&g_gates->held, KOS_TIMEOUT_NONE) == 0
                   and holder.cancel(KOS_CANCEL_KILL, 0) == 0
                   and kos_sem_post(&g_gates->go) == 0 and setter.join() == 0;
        if (ran)
        {
            auto taker = spawn_parked(malloc_victim, "mtaker", core_mask);
            ran = taker.valid() and taker.join() == 0 and holder.join() == 0;
        }
        unpin_main(core_mask);
        if (ran)
        {
            return 0;
        }
        return REFUSED;
    }
#endif

#if KICKOS_FAULT_ISOLATION and KICKOS_HAVE_ASPACE
    // A holder of the malloc lock is killed, with a taker parked behind it or, `waiting` false,
    // one that comes only after the death, so the taker takes the lock from a dead owner and
    // ends the task.
    int dead_malloc_holder_ends_the_takers_task(bool waiting)
    {
        uint32_t core_mask = 0;
        if (not pin_main(&core_mask))
        {
            return REFUSED;
        }
        auto holder = spawn_in(dead_malloc_holder, nullptr, "mholder", WORKER_PRIO, 0);
        bool ran = holder.valid() and kos_sem_wait(&g_gates->held, KOS_TIMEOUT_NONE) == 0;
        if (ran and waiting)
        {
            auto taker = spawn_parked(malloc_victim, "mwaiter", core_mask);
            ran = taker.valid() and holder.cancel(KOS_CANCEL_KILL, 0) == 0 and holder.join() == 0
                  and taker.join() == 0;
        }
        else if (ran)
        {
            ran = holder.cancel(KOS_CANCEL_KILL, 0) == 0 and holder.join() == 0;
            auto taker = spawn_parked(malloc_victim, "mlater", core_mask);
            ran = ran and taker.valid() and taker.join() == 0;
        }
        unpin_main(core_mask);
        if (ran)
        {
            return 0;
        }
        return REFUSED;
    }
#endif

    // Having spawned fewer than TASK_RACERS, releases the ones it has, and main's racers, which
    // wait on MEET_OTHER.
    void release_racers(uint32_t spawned)
    {
        if (spawned < TASK_RACERS)
        {
            for (uint32_t k = 0; k < spawned; k++)
            {
                kos_sem_post(&g_gates->begun);
            }
            (void)kos_notify(MEET_OTHER);
        }
    }

    // The racing workers the peer's task holds, each printing and allocating until done.
    int racers()
    {
        uint32_t started = 0;
        for (int w = WORKERS / 2; w < WORKERS; w++)
        {
            if (spawn_racer(worker, w, MEET_OWN, MEET_OTHER).valid())
            {
                started++;
            }
        }
        release_racers(started);
        for (uint32_t w = 0; w < started; w++)
        {
            kos_sem_wait(&g_gates->done, KOS_TIMEOUT_NONE);
        }
        if (started != TASK_RACERS)
        {
            return REFUSED;
        }
        return 0;
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        return REFUSED;
    }
    gates_seat();
    char const* const role = argv[1];
#if defined(_NEWLIB_VERSION)
    if (strcmp(role, "release") == 0)
    {
        return running_killed_holder_releases();
    }
#endif
#if KICKOS_FAULT_ISOLATION and KICKOS_HAVE_ASPACE
    if (strcmp(role, "dead_waiting") == 0)
    {
        return dead_malloc_holder_ends_the_takers_task(true);
    }
    if (strcmp(role, "dead_later") == 0)
    {
        return dead_malloc_holder_ends_the_takers_task(false);
    }
#endif
    if (strcmp(role, "racers") == 0)
    {
        return racers();
    }
    return REFUSED;
}
