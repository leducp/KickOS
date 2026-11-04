// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A thread killed inside printf, holding the libc lock, and the lock's next taker. Main holds
// malloc's lock, so the victim's printf, the first use of stdout, parks inside stdio on the
// allocation of stdout's buffer with the libc lock held, and is killed there. Main then frees
// malloc's lock and calls getenv, which takes the libc lock from its dead holder: that ends
// main's task, or on a chip that holds one task the whole image, and the line after it never
// prints. Nothing before the victim's printf may use stdout.

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
    kos_sem_t g_printed = KOS_SEM_INIT(0);

    void victim(void*)
    {
        kos_sem_post(&g_entering);
        printf("libc_death: the victim printed\n");
        kos_sem_post(&g_printed);
    }

    bool malloc_waited()
    {
        kos_mutex_t const* const m = kos_libc_lock(KOS_LIBC_LOCK_MALLOC);
        return (__atomic_load_n(&m->word, __ATOMIC_ACQUIRE) & KOS_WORD_WAITERS) != 0;
    }

    // True once the victim waits on malloc's lock inside its printf, with the libc lock held.
    bool parked_inside_printf()
    {
        for (int i = 0; i < PARK_POLLS; i++)
        {
            if (malloc_waited())
            {
                return true;
            }
            if (kos_sem_wait(&g_printed, POLL_US) == 0)
            {
                return false;
            }
        }
        return false;
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
    bool const parked = parked_inside_printf();
    bool const killed = v.cancel(KOS_CANCEL_KILL, 0) == 0 and v.join() == 0;
    __malloc_unlock(_REENT);
    if (not parked or not killed)
    {
        kos::print("libc_death: the victim never died inside printf\n");
        return 1;
    }
    kos::print("libc_death: the victim died inside printf holding the libc lock\n");
    char const* volatile env = getenv("KICKOS_LIBC_DEATH_UNSET");
    (void)env;
    kos::print("libc_death: the next taker of the libc lock went on\n");
    return 0;
}
