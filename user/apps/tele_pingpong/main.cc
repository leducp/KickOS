// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Telemetry structural gate workload (spike CI gates 3 + 4). Exercises every
// telemetry hook: real ping-pong (context switches + semaphore syscalls) and a
// multi-wake-in-one-ISR (several periodic sleepers whose deadlines coalesce, so
// one timer ISR wakes them together -> many reschedules collapse to a SINGLE
// physical switch). The workers are DAEMONS (they never return); only the root
// thread ends -- by returning from main, which makes the boot path call
// arch_shutdown directly. This deliberately avoids a spawned thread calling
// exit(): on the ARM port a non-last thread exit is currently broken (the
// deferred PendSV switch cannot fire under exit_current's held IrqLock), a
// pre-existing scheduler limitation unrelated to telemetry. arch_shutdown flushes
// the ch1 ring (sim: to a file; qemu: via semihosting) for the decoder to assert.

#include <kickos/kos.h>

namespace
{
    constexpr int kSleepers = 3;
    constexpr uint64_t kBeatNs = 1000000ull; // 1 ms
    constexpr uint64_t kRunNs = 40000000ull; // main lets the workers run ~40 ms

    kos::Semaphore* g_ping = nullptr;
    kos::Semaphore* g_pong = nullptr;

    // B1 well-known child cap indices: (ping_s, pong_s) delegated -> ping@1, pong@2.
    constexpr int CH_PING = 1;
    constexpr int CH_PONG = 2;
    constexpr uint8_t CH_FULL = KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER;

    void ping(void*)
    {
        while (true)
        {
            kos_sem_wait(CH_PING);
            kos::sleep_ns(kBeatNs);
            kos_sem_post(CH_PONG);
        }
    }
    void pong(void*)
    {
        while (true)
        {
            kos_sem_wait(CH_PONG);
            kos::sleep_ns(kBeatNs);
            kos_sem_post(CH_PING);
        }
    }
    // Periodic sleeper: fixed-duration sleeps whose deadlines coalesce with the
    // peers' (especially under QEMU's coarse trace clock) -> timer multi-wake.
    void sleeper(void*)
    {
        while (true)
        {
            kos::sleep_ns(2000000ull); // 2 ms
        }
    }
}

int main(int, char**)
{
    kos::print("tele_pingpong: bounded telemetry workload\n");

    kos::Semaphore ping_s(1); // ping serves first
    kos::Semaphore pong_s(0);
    g_ping = &ping_s;
    g_pong = &pong_s;

    kos_cap_grant caps[] = {{ping_s.id(), CH_FULL}, {pong_s.id(), CH_FULL}}; // ping@1, pong@2
    kos::thread::spawn_caps(ping, nullptr, "ping", 10, caps, 2);
    kos::thread::spawn_caps(pong, nullptr, "pong", 10, caps, 2);
    for (int i = 0; i < kSleepers; i++)
    {
        kos::thread::spawn(sleeper, nullptr, "sleeper", 5);
    }

    // Let the daemons run for a bounded time (root is prio 2, below the workers,
    // so it runs only while they are all blocked -- then this sleep expires and we
    // return, ending the run cleanly through the boot path's arch_shutdown).
    kos::sleep_ns(kRunNs);
    kos::print("tele_pingpong: done\n");
    return 0;
}
