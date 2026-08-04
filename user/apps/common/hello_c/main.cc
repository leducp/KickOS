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

    kos_cap_t g_ping = KOS_CAP_NONE; // token held by 'ping' first (MAIN's cap)
    kos_cap_t g_pong = KOS_CAP_NONE;

    // B1 well-known child cap indices: both sems delegated (g_ping, g_pong) per spawn
    // -> ping@1, pong@2 (fresh child table => handle == index).
    constexpr kos_cap_t CH_PING = 1;
    constexpr kos_cap_t CH_PONG = 2;

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
        // A null out-pointer is refused -KOS_EINVAL, so the sink is not optional even
        // though nothing here reads it.
        kos_thread_t h = KOS_THREAD_NONE;
        return kos_thread_spawn(&p, &h);
    }
}

int main(int, char**)
{
    kos_print("hello from KickOS userspace!\n");
    kos_print("two threads play ping-pong -- press Ctrl+C to stop.\n\n");

    (void)kos_sem_create(1, &g_ping); // ping serves first
    (void)kos_sem_create(0, &g_pong);

    spawn(ping, "ping");
    spawn(pong, "pong");

    // A daemon: main never returns; park forever on a semaphore nobody posts.
    kos_cap_t idle = KOS_CAP_NONE;
    (void)kos_sem_create(0, &idle);
    while (true)
    {
        kos_sem_wait(idle);
    }
}
