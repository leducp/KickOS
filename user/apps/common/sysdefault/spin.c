// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The default system's main returning while a thread it created spins, never entering the
// kernel: the init ends the system on the ENDED bit and does not wait for the spinner to die.
// The spinner runs below the init's priority, which a thread on the init's core must for the
// init to run at all.

#include <kickos/config/priorities.h>
#include <kickos/sys.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static uint32_t g_spinning;

static void spin(void* arg)
{
    (void)arg;
    __atomic_store_n(&g_spinning, 1u, __ATOMIC_RELAXED);
    while (true)
    {
    }
}

int main(int argc, char** argv)
{
    struct kos_thread_params p = {0};
    kos_thread_t thread = KOS_THREAD_NONE;
    int rc = 0;
    int waited = 0;

    (void)argc;
    (void)argv;
    p.entry = spin;
    p.name = "spinner";
    p.prio = KICKOS_PRIO_MIN;
    rc = kos_thread_create(&p, &thread);
    if (rc != 0)
    {
        puts("sysdefault: spinner refused");
        return 1;
    }
    // main sleeps, so the spinner below it runs.
    while (__atomic_load_n(&g_spinning, __ATOMIC_RELAXED) == 0u and waited < 1000)
    {
        kos_sleep_ns(1000000ull);
        waited++;
    }
    if (__atomic_load_n(&g_spinning, __ATOMIC_RELAXED) == 0u)
    {
        puts("sysdefault: spinner never ran");
        return 1;
    }
    puts("sysdefault: main returns 3 while a thread spins");
    return 3;
}
