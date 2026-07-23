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
    constexpr uint64_t BEAT_NS = 400000000ull; // 0.4 s between hits

    // Shared by both players; bound in main() once the kernel is up. (A global
    // kos::Semaphore would run its ctor -- a syscall -- before the scheduler.)
    kos::Semaphore* g_ping = nullptr;
    kos::Semaphore* g_pong = nullptr;

    // B1 well-known child cap indices (fresh child table => handle == index). Both sems
    // are delegated to each player in the order (ping_sem, pong_sem) -> ping@1, pong@2.
    constexpr int CH_PING = 1;
    constexpr int CH_PONG = 2;
    constexpr uint8_t CH_FULL = KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER;

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
            kos_sem_wait(CH_PING);
            kos::sleep_ns(BEAT_NS);
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
            kos::sleep_ns(BEAT_NS);
            say("pong", ++n);
            kos_sem_post(CH_PING);
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

    // Both players get (ping_sem, pong_sem) delegated -> ping@1, pong@2 (CH_PING/CH_PONG).
    kos_cap_grant caps[] = {{ping_sem.id(), CH_FULL}, {pong_sem.id(), CH_FULL}};
    kos::thread::spawn_caps(ping, nullptr, "ping", 10, caps, 2);
    kos::thread::spawn_caps(pong, nullptr, "pong", 10, caps, 2);

    // A daemon: main never returns (returning would exit), so the two players
    // run until interrupted. Park at low priority on a semaphore nobody posts.
    kos::Semaphore idle(0);
    while (true)
    {
        idle.wait();
    }
}
