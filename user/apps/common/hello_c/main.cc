// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KickOS "hello_c" (C flavor): the same ping-pong demo as apps/hello, against
// the plain-C syscall API (kos_*). Both funnel through the identical syscall
// trap; this is a style comparison, not a migration.

#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

namespace
{
    constexpr uint64_t BEAT_NS = 400000000ull; // 0.4 s between hits

    int g_ping = -1; // token held by 'ping' first (MAIN's cap)
    int g_pong = -1;

    // B1 well-known child cap indices: both sems delegated (g_ping, g_pong) per spawn
    // -> ping@1, pong@2 (fresh child table => handle == index).
    constexpr int CH_PING = 1;
    constexpr int CH_PONG = 2;

    void say(char const* who, int n)
    {
        char b[48];
        ksnprintf(b, sizeof(b), "  %s %d\n", who, n);
        kos_print(b);
    }

    void ping(void*)
    {
        int n = 0;
        while (true)
        {
            kos_sem_wait(CH_PING);
            kos_sleep_ns(BEAT_NS);
            say("ping", ++n);
            kos_sem_post(CH_PONG);
        }
    }
    void pong(void*)
    {
        int n = 0;
        while (true)
        {
            kos_sem_wait(CH_PONG);
            kos_sleep_ns(BEAT_NS);
            say("pong", ++n);
            kos_sem_post(CH_PING);
        }
    }

    int spawn(void (*entry)(void*), char const* name)
    {
        kos_cap_grant caps[] = {{g_ping, KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER},
                                {g_pong, KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER}};
        kos_thread_params p = {};
        p.entry = entry;
        p.name = name;
        p.prio = 10;
        p.caps = caps;
        p.cap_count = 2;
        return kos_thread_spawn(&p);
    }
}

int main(int, char**)
{
    kos_print("hello from KickOS userspace!\n");
    kos_print("two threads play ping-pong -- press Ctrl+C to stop.\n\n");

    g_ping = kos_sem_create(1); // ping serves first
    g_pong = kos_sem_create(0);

    spawn(ping, "ping");
    spawn(pong, "pong");

    // A daemon: main never returns; park forever on a semaphore nobody posts.
    int idle = kos_sem_create(0);
    while (true)
    {
        kos_sem_wait(idle);
    }
}
