// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The kernel clock held against a clock outside the chip: three marks a requested sleep apart,
// which tests/integration/check_wallclock.sh times between the second and third by the capture
// host's own arrival stamps. A USB device console loses the first. Then the steady clock across a
// wall-clock set a day ahead, made between its two reads: it advances by the sleep alone. Last, the
// CPU clock as a static constructor read it and as main reads it.

#include <kickos/kos.h>
#include <kickos/libc/fmt.h>

#include <stdint.h>

#include <chrono>

namespace
{
    constexpr uint64_t SLEEP_NS = 5000000000ull;

    // The kernel time one sleep takes, read on either side of the sleep alone.
    uint64_t timed_sleep()
    {
        uint64_t const from = kos::clock_now();
        kos::sleep_ns(SLEEP_NS);
        return kos::clock_now() - from;
    }

    constexpr uint64_t DAY_NS = 86400000000000ull;
    constexpr uint64_t SET_SLEEP_NS = 1000000000ull;

    struct AcrossSet
    {
        int set;
        uint64_t steady;
        uint64_t wall;
    };

    AcrossSet steady_across_set()
    {
        AcrossSet r;
        uint64_t const wall_from = kos::clock_realtime();
        auto const from = std::chrono::steady_clock::now();
        r.set = kos::clock_set_realtime(wall_from + DAY_NS);
        kos::sleep_ns(SET_SLEEP_NS);
        auto const to = std::chrono::steady_clock::now();
        r.wall = kos::clock_realtime() - wall_from;
        r.steady = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(to - from).count());
        return r;
    }

    struct CtorClock
    {
        uint64_t hz;
        CtorClock() : hz(kos::cpu_clock_hz()) {}
    };
    CtorClock const g_ctor_clock;
}

int main(int, char**)
{
    char line[192];
    ksnprintf(line, sizeof(line), "[wallclock] mark 0, sleeping %llu ns\n",
              static_cast<unsigned long long>(SLEEP_NS));
    kos::print(line);
    uint64_t took = timed_sleep();
    ksnprintf(line, sizeof(line),
              "[wallclock] mark 1, the kernel clock advanced %llu ns, sleeping %llu ns\n",
              static_cast<unsigned long long>(took), static_cast<unsigned long long>(SLEEP_NS));
    kos::print(line);
    took = timed_sleep();
    ksnprintf(line, sizeof(line), "[wallclock] mark 2, the kernel clock advanced %llu ns\n",
              static_cast<unsigned long long>(took));
    kos::print(line);
    AcrossSet const across = steady_across_set();
    ksnprintf(line, sizeof(line),
              "[wallclock] mark 3, a set a day ahead answered %d, the steady clock advanced %llu ns"
              " over a sleep of %llu ns, the wall clock %llu ns\n",
              across.set, static_cast<unsigned long long>(across.steady),
              static_cast<unsigned long long>(SET_SLEEP_NS),
              static_cast<unsigned long long>(across.wall));
    kos::print(line);
    ksnprintf(line, sizeof(line),
              "[wallclock] the CPU clock: %llu Hz read by a constructor, %llu Hz by main\n",
              static_cast<unsigned long long>(g_ctor_clock.hz),
              static_cast<unsigned long long>(kos::cpu_clock_hz()));
    kos::print(line);
    kos::print("[wallclock] done\n");
    return 0;
}
