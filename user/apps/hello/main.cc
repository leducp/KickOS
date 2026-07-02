// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KickOS "hello": a small, friendly userspace demo. Two unprivileged threads
// bounce a token back and forth over a pair of semaphores (ping-pong), pausing
// between hits so it is watchable, and run until you stop the sim with Ctrl+C.
//
// (The exhaustive M0 verification lives in apps/selftest, not here.)

#include <kickos/kos.hpp>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>
#include <kickos/libc/string.h>

namespace
{

    constexpr uint64_t kBeatNs = 400000000ull; // 0.4 s between hits

    int g_ping = -1; // token held by 'ping' first
    int g_pong = -1; // token held by 'pong'

    void line(char const* s)
    {
        kos_write(1, s, strlen(s));
    }

    void say(char const* who, int n)
    {
        char b[48];
        ksnprintf(b, sizeof(b), "  %s %d\n", who, n);
        line(b);
    }

    // Bounce forever: wait for my token, pause, speak, hand it to my peer.
    void ping(void*)
    {
        int n = 0;
        while (true)
        {
            kos_sem_wait(g_ping);
            kos_sleep_ns(kBeatNs);
            say("ping", ++n);
            kos_sem_post(g_pong);
        }
    }
    void pong(void*)
    {
        int n = 0;
        while (true)
        {
            kos_sem_wait(g_pong);
            kos_sleep_ns(kBeatNs);
            say("pong", ++n);
            kos_sem_post(g_ping);
        }
    }

}

extern "C" void kickos_app_main(void)
{
    line("hello from KickOS userspace!\n");
    line("two threads play ping-pong -- press Ctrl+C to stop.\n\n");

    g_ping = kos_sem_create(1); // ping serves first
    g_pong = kos_sem_create(0);

    kos::spawn(ping, nullptr, "ping", 10);
    kos::spawn(pong, nullptr, "pong", 10);
    // root returns; the two players run until the sim is interrupted.
}
