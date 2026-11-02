// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Spawns parked threads until the kernel refuses one, then prints how many it took and the
// refusal: the thread pool's size as the image's system sized it, less what was already live.

#include <stdbool.h>

#include <kickos/libc/fmt.h>
#include <kickos/sys.h>

static kos_sem_t g_never = KOS_SEM_INIT(0);

static void parked(void* arg)
{
    (void)arg;
    while (true)
    {
        kos_sem_wait(&g_never, KOS_TIMEOUT_NONE);
    }
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    int spawned = 0;
    int rc = 0;
    while (rc == 0)
    {
        struct kos_thread_params p = {0};
        kos_thread_t h = KOS_THREAD_NONE;
        p.entry = parked;
        p.name = "parked";
        p.prio = 1;
        rc = kos_thread_create(&p, &h);
        if (rc == 0)
        {
            spawned++;
        }
    }
    char line[64];
    ksnprintf(line, sizeof(line), "pool-abi: %d threads, then %d\n", spawned, rc);
    kos_print(line);
    return 0;
}
