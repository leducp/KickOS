// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Telemetry structural gate workload. Exercises every telemetry hook: real ping-pong
// (context switches + semaphore syscalls) and a multi-wake-in-one-ISR (several
// periodic sleepers whose deadlines coalesce, so one timer ISR wakes them together
// -> many reschedules collapse to a SINGLE physical switch). The workers are DAEMONS
// (they never return); only main ends, by returning, which ends its task, the workers with
// it, and the system through the init. arch_shutdown flushes the ch1 ring (sim: to a file;
// qemu: via semihosting) for the decoder to assert.

#include <kickos/kos.h>

namespace
{
    constexpr int SLEEPERS = 3;
    constexpr uint64_t BEAT_NS = 1000000ull; // 1 ms
    constexpr uint64_t RUN_NS = 40000000ull; // main lets the workers run ~40 ms

    kos::Semaphore g_ping(1); // ping serves first
    kos::Semaphore g_pong;

    void ping(void*)
    {
        while (true)
        {
            g_ping.wait();
            kos::sleep_ns(BEAT_NS);
            g_pong.post();
        }
    }
    void pong(void*)
    {
        while (true)
        {
            g_pong.wait();
            kos::sleep_ns(BEAT_NS);
            g_ping.post();
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

    kos::thread::create(ping, nullptr, "ping", 10);
    kos::thread::create(pong, nullptr, "pong", 10);
    for (int i = 0; i < SLEEPERS; i++)
    {
        kos::thread::create(sleeper, nullptr, "sleeper", 5);
    }

    // Let the daemons run for a bounded time (main runs at prio 2, below the workers,
    // so it runs only while they are all blocked; then this sleep expires and we
    // return, ending the run cleanly through the boot path's arch_shutdown).
    kos::sleep_ns(RUN_NS);
    kos::print("tele_pingpong: done\n");
    return 0;
}
