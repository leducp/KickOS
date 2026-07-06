// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KickOS "hello" (C++ flavor). Two unprivileged threads bounce a token back and
// forth over a pair of semaphores (ping-pong), pausing between hits so it is
// watchable, until you stop the sim with Ctrl+C. Written against the ergonomic
// C++ API (kos::). The byte-for-byte identical program in the plain-C API lives
// in apps/hello_c -- compare the two: same syscalls, different call flavor.
//
// (The exhaustive M0 verification lives in apps/selftest, not here.)

#include <kickos/kos.h>
#include <kickos/libc/fmt.h>

namespace
{
    constexpr uint64_t kBeatNs = 400000000ull; // 0.4 s between hits

    // Shared by both players; bound in main() once the kernel is up. (A global
    // kos::Semaphore would run its ctor -- a syscall -- before the scheduler.)
    kos::Semaphore* g_ping = nullptr;
    kos::Semaphore* g_pong = nullptr;

    void say(char const* who, int n)
    {
        char b[48];
        ksnprintf(b, sizeof(b), "  %s %d\n", who, n);
        kos::print(b);
    }

    // Bounce forever: wait for my token, pause, speak, hand it to my peer.
    void ping(void*)
    {
        int n = 0;
        while (true)
        {
            g_ping->wait();
            kos::sleep_ns(kBeatNs);
            say("ping", ++n);
            g_pong->post();
        }
    }
    void pong(void*)
    {
        int n = 0;
        while (true)
        {
            g_pong->wait();
            kos::sleep_ns(kBeatNs);
            say("pong", ++n);
            g_ping->post();
        }
    }
}

int main(int, char**)
{
    kos::print("hello from KickOS userspace!\n");
    kos::print("two threads play ping-pong -- press Ctrl+C to stop.\n\n");

    kos::Semaphore ping_sem(1); // ping serves first
    kos::Semaphore pong_sem(0);
    g_ping = &ping_sem;
    g_pong = &pong_sem;

    kos::thread::spawn(ping, nullptr, "ping", 10);
    kos::thread::spawn(pong, nullptr, "pong", 10);

    // A daemon: main never returns (returning would exit), so the two players
    // run until interrupted. Park at low priority on a semaphore nobody posts.
    kos::Semaphore idle(0);
    while (true)
    {
        idle.wait();
    }
}
