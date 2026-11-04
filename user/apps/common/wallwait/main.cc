// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A thread waits on system_clock an hour ahead, through a condition variable and then a timed
// mutex, while main sets the wall clock a day ahead: each wait ends timed out, after the set and
// promptly. tests/integration/check_wallwait.sh judges the lines.

#include <kickos/kos.h>
#include <kickos/libc/fmt.h>

#include <stdint.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace
{
    using steady = std::chrono::steady_clock;
    using wall_clock = std::chrono::system_clock;

    constexpr uint64_t DAY_NS = 86400000000000ull;
    // Main's sleep between starting the waiter and its set; the waiter parks inside it.
    constexpr uint64_t SETTLE_NS = 300000000ull;

    std::mutex g_m;
    std::condition_variable g_cv;
    std::timed_mutex g_held;

    bool g_timed_out = false;
    steady::time_point g_ended;

    void cv_waiter()
    {
        std::unique_lock<std::mutex> lock(g_m);
        wall_clock::time_point const until = wall_clock::now() + std::chrono::hours(1);
        std::cv_status const s = g_cv.wait_until(lock, until);
        g_ended = steady::now();
        g_timed_out = s == std::cv_status::timeout;
    }

    void lock_waiter()
    {
        wall_clock::time_point const until = wall_clock::now() + std::chrono::hours(1);
        bool const got = g_held.try_lock_until(until);
        g_ended = steady::now();
        g_timed_out = not got;
        if (got)
        {
            g_held.unlock();
        }
    }

    void across_set(char const* what, void (*waiter)())
    {
        std::thread t(waiter);
        kos::sleep_ns(SETTLE_NS);
        steady::time_point const set_at = steady::now();
        int const rc = kos::clock_set_realtime(kos::clock_realtime() + DAY_NS);
        t.join();
        long long const after = static_cast<long long>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(g_ended - set_at).count());
        char const* timed_out = "no";
        if (g_timed_out)
        {
            timed_out = "yes";
        }
        char line[192];
        ksnprintf(line, sizeof(line),
                  "[wallwait] %s an hour ahead: a set a day ahead answered %d, timed out %s, "
                  "ended %lld ns after the set\n",
                  what, rc, timed_out, after);
        kos::print(line);
    }
}

int main(int, char**)
{
    across_set("condition_variable wait_until", cv_waiter);
    g_held.lock();
    across_set("timed_mutex try_lock_until", lock_waiter);
    g_held.unlock();
    kos::print("[wallwait] done\n");
    return 0;
}
