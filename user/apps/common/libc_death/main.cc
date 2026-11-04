// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A thread killed inside libc holding one of its locks, and that lock's next taker. Main holds
// malloc's lock, so the victim parks on an allocation inside libc with the other lock held, and
// is killed there: inside printf, the first use of stdout, holding stdout's lock while it asks
// for stdout's buffer; or, built with KICKOS_LIBC_DEATH_ENV, inside setenv, holding the
// environment's lock while it grows the environment. Main then frees malloc's lock and takes the
// dead victim's lock, which ends main's task, and the line after it never prints. Nothing before the victim's printf may use stdout.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>

namespace
{
    constexpr uint8_t VICTIM_PRIO = 10;
    // How long main waits for the victim to park behind malloc's lock, in 1 ms steps.
    constexpr int PARK_POLLS = 2000;
    constexpr uint32_t POLL_US = 1000;

    kos_sem_t g_entering = KOS_SEM_INIT(0);
    kos_sem_t g_returned = KOS_SEM_INIT(0);

#if defined(KICKOS_LIBC_DEATH_ENV)
    constexpr char const* NEVER_DIED = "libc_death: the victim never died inside setenv\n";
    constexpr char const* DIED =
        "libc_death: the victim died inside setenv holding the environment's lock\n";
    constexpr char const* WENT_ON =
        "libc_death: the next taker of the environment's lock went on\n";
#else
    constexpr char const* NEVER_DIED = "libc_death: the victim never died inside printf\n";
    constexpr char const* DIED =
        "libc_death: the victim died inside printf holding stdout's lock\n";
    constexpr char const* WENT_ON = "libc_death: the next taker of stdout's lock went on\n";
#endif

    void victim(void*)
    {
        kos_sem_post(&g_entering);
#if defined(KICKOS_LIBC_DEATH_ENV)
        (void)setenv("KICKOS_LIBC_DEATH", "victim", 1);
        kos::print("libc_death: the victim set the environment\n");
#else
        printf("libc_death: the victim printed\n");
#endif
        kos_sem_post(&g_returned);
    }

    bool malloc_waited()
    {
        kos_mutex_t const* const m = kos_libc_malloc_lock();
        return (__atomic_load_n(&m->word, __ATOMIC_ACQUIRE) & KOS_WORD_WAITERS) != 0;
    }

    // True once the victim waits on malloc's lock inside its call, with the other lock held.
    bool parked_inside_the_call()
    {
        for (int i = 0; i < PARK_POLLS; i++)
        {
            if (malloc_waited())
            {
                return true;
            }
            if (kos_sem_wait(&g_returned, POLL_US) == 0)
            {
                return false;
            }
        }
        return false;
    }

    // Takes the dead victim's lock.
    void next_taker()
    {
#if defined(KICKOS_LIBC_DEATH_ENV)
        char const* volatile env = getenv("KICKOS_LIBC_DEATH_UNSET");
        (void)env;
#else
        fputs("libc_death: main printed\n", stdout);
#endif
    }
}

int main(int, char**)
{
    kos::print("libc_death: start\n");
    __malloc_lock(_REENT);
    auto v = kos::thread::create(victim, nullptr, "victim", VICTIM_PRIO);
    if (not v.valid() or kos_sem_wait(&g_entering, KOS_TIMEOUT_NONE) != 0)
    {
        __malloc_unlock(_REENT);
        kos::print("libc_death: the victim was refused\n");
        return 1;
    }
    bool const parked = parked_inside_the_call();
    bool const killed = v.cancel(KOS_CANCEL_KILL, 0) == 0 and v.join() == 0;
    __malloc_unlock(_REENT);
    if (not parked or not killed)
    {
        kos::print(NEVER_DIED);
        return 1;
    }
    kos::print(DIED);
    next_taker();
    kos::print(WENT_ON);
    return 0;
}
