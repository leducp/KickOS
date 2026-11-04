// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The real user/src/gthr.cc over a scripted kernel: what it hands the kernel for an absolute
// deadline, a timeout on the monotonic clock and the deadline itself on the realtime one, what each
// timed wait answers as the kernel and the clocks move, a wall-clock set included, and the thread
// id of root.

#include <gtest/gtest.h>

#include <cstdlib>
#include <deque>
#include <limits>
#include <string>

#include <time.h>

#include <kickos/reent.h>
#include <kickos/sys.h>

namespace
{
    struct Mutex
    {
        uint32_t word;
    };

    struct RecursiveMutex
    {
        uint32_t word;
        uint32_t depth;
    };

    struct Cond
    {
        uint32_t seq;
    };

    timespec g_now = {};
    uint64_t g_mono_ns = 0;
    uint32_t g_calls = 0;
    uint32_t g_handed_us = 0;
    // The last kernel wait's realtime deadline, where it was one.
    bool g_handed_on_wall = false;
    uint64_t g_handed_wall = 0;
    KickosThreadWords g_words = {};
    kos_thread_t g_self = 1u;

    // One kernel wait's answer, and how far it moves the clocks: by the timeout it was handed
    // when `waits`, and the realtime clock by `wall_step_s` more.
    struct Answer
    {
        int rc;
        bool waits;
        int64_t wall_step_s;
    };

    // Answers in order; once empty, every wait is granted at once.
    std::deque<Answer> g_script;

    uint64_t wall_now_ns()
    {
        return static_cast<uint64_t>(g_now.tv_sec) * 1000000000u
               + static_cast<uint64_t>(g_now.tv_nsec);
    }

    void advance(uint64_t ns)
    {
        g_mono_ns += ns;
        uint64_t const wall = wall_now_ns() + ns;
        g_now.tv_sec = static_cast<time_t>(wall / 1000000000u);
        g_now.tv_nsec = static_cast<long>(wall % 1000000000u);
    }

    // A realtime kernel wait: one that `waits` lasts until the wall clock reads its deadline.
    int answer_wall(uint64_t unix_ns)
    {
        g_calls++;
        g_handed_on_wall = true;
        g_handed_wall = unix_ns;
        if (g_script.empty())
        {
            return 0;
        }
        Answer const a = g_script.front();
        g_script.pop_front();
        g_now.tv_sec += static_cast<time_t>(a.wall_step_s);
        if (a.waits and unix_ns > wall_now_ns())
        {
            advance(unix_ns - wall_now_ns());
        }
        return a.rc;
    }

    int answer(uint32_t timeout_us)
    {
        g_calls++;
        g_handed_on_wall = false;
        g_handed_us = timeout_us;
        if (g_script.empty())
        {
            return 0;
        }
        Answer const a = g_script.front();
        g_script.pop_front();
        if (a.waits)
        {
            uint64_t const ns = static_cast<uint64_t>(timeout_us) * 1000u;
            g_mono_ns += ns;
            g_now.tv_sec += static_cast<time_t>(ns / 1000000000u);
            g_now.tv_nsec += static_cast<long>(ns % 1000000000u);
            if (g_now.tv_nsec >= 1000000000)
            {
                g_now.tv_sec++;
                g_now.tv_nsec -= 1000000000;
            }
        }
        g_now.tv_sec += static_cast<time_t>(a.wall_step_s);
        return a.rc;
    }
}

// gthr.cc's operations, named here without the toolchain's types, which the host's own threads
// model would clash with.
extern "C"
{
    int __kickos_gthr_mutex_timedlock(Mutex* m, timespec const* abs);
    int __kickos_gthr_mutex_clocklock(Mutex* m, clockid_t clock, timespec const* abs);
    int __kickos_gthr_recursive_mutex_timedlock(RecursiveMutex* m, timespec const* abs);
    int __kickos_gthr_recursive_mutex_clocklock(RecursiveMutex* m, clockid_t clock,
                                                timespec const* abs);
    int __kickos_gthr_cond_timedwait(Cond* c, Mutex* m, timespec const* abs);
    int __kickos_gthr_cond_clockwait(Cond* c, Mutex* m, clockid_t clock, timespec const* abs);
    uint32_t __kickos_gthr_self(void);
    int __kickos_gthr_join(uint32_t thread, void** value);

    int __real_clock_gettime(clockid_t clock, timespec* ts);
    int __wrap_clock_gettime(clockid_t clock, timespec* ts);
}

extern "C" int __wrap_clock_gettime(clockid_t clock, timespec* ts)
{
    if (clock != CLOCK_REALTIME)
    {
        return __real_clock_gettime(clock, ts);
    }
    *ts = g_now;
    return 0;
}

extern "C" uint64_t kos_clock_now(void)
{
    return g_mono_ns;
}

extern "C" int kos_mutex_lock(kos_mutex_t*, uint32_t timeout_us)
{
    return answer(timeout_us);
}

extern "C" int kickos_mutex_lock_entry(kos_mutex_t*, uint32_t timeout_us)
{
    return answer(timeout_us);
}

extern "C" int kos_mutex_lock_realtime(kos_mutex_t*, uint64_t unix_ns)
{
    return answer_wall(unix_ns);
}

extern "C" int kickos_mutex_lock_entry_realtime(kos_mutex_t*, uint64_t unix_ns)
{
    return answer_wall(unix_ns);
}

extern "C" int kos_mutex_unlock(kos_mutex_t*)
{
    return 0;
}

extern "C" uint32_t kickos_self_lock_id(void)
{
    return 1u;
}

extern "C" int kos_cond_wait(kos_cond_t*, kos_mutex_t*, uint32_t timeout_us)
{
    return answer(timeout_us);
}

extern "C" int kos_cond_wait_realtime(kos_cond_t*, kos_mutex_t*, uint64_t unix_ns)
{
    return answer_wall(unix_ns);
}

extern "C" int kos_cond_signal(kos_cond_t*)
{
    return 0;
}

extern "C" int kos_cond_broadcast(kos_cond_t*)
{
    return 0;
}

extern "C" int kos_thread_create(kos_thread_params const*, kos_thread_t*)
{
    return -KOS_ENOMEM;
}

extern "C" int kos_thread_join(kos_thread_t, uint32_t)
{
    return 0;
}

extern "C" kos_thread_t kos_thread_self(void)
{
    return g_self;
}

extern "C" void kos_exit(int)
{
    std::abort();
}

extern "C" void kos_yield(void)
{
}

extern "C" void kickos_thread_dtors_run(void)
{
}

extern "C" KickosThreadWords* kickos_thread_words(void)
{
    return &g_words;
}

namespace
{
    enum class Wait
    {
        MUTEX,
        RECURSIVE,
        COND,
    };

    // TIMED is the timedlock or timedwait call, which names no clock and waits on the realtime
    // one; MONOTONIC and REALTIME are the clocklock or clockwait call naming that clock.
    enum class Clock
    {
        TIMED,
        MONOTONIC,
        REALTIME,
    };

    struct Case
    {
        Wait wait;
        Clock clock;
    };

    constexpr uint32_t CEILING_US = KOS_TIMEOUT_NONE - 1u;

    void reset()
    {
        g_now = {100, 0};
        g_mono_ns = 5000000000u;
        g_calls = 0;
        g_handed_us = 0;
        g_handed_on_wall = false;
        g_handed_wall = 0;
        g_script.clear();
    }

    timespec at(uint64_t ns)
    {
        return {static_cast<time_t>(ns / 1000000000u), static_cast<long>(ns % 1000000000u)};
    }

    uint64_t wall_ns()
    {
        return static_cast<uint64_t>(g_now.tv_sec) * 1000000000u
               + static_cast<uint64_t>(g_now.tv_nsec);
    }

    int wait_until(Case c, timespec const& abs)
    {
        Mutex m = {};
        RecursiveMutex r = {};
        Cond cv = {};
        clockid_t clock = CLOCK_REALTIME;
        if (c.clock == Clock::MONOTONIC)
        {
            clock = CLOCK_MONOTONIC;
        }
        if (c.wait == Wait::MUTEX)
        {
            if (c.clock == Clock::TIMED)
            {
                return __kickos_gthr_mutex_timedlock(&m, &abs);
            }
            return __kickos_gthr_mutex_clocklock(&m, clock, &abs);
        }
        if (c.wait == Wait::RECURSIVE)
        {
            if (c.clock == Clock::TIMED)
            {
                return __kickos_gthr_recursive_mutex_timedlock(&r, &abs);
            }
            return __kickos_gthr_recursive_mutex_clocklock(&r, clock, &abs);
        }
        if (c.clock == Clock::TIMED)
        {
            return __kickos_gthr_cond_timedwait(&cv, &m, &abs);
        }
        return __kickos_gthr_cond_clockwait(&cv, &m, clock, &abs);
    }

    class GthrTimed : public ::testing::TestWithParam<Case>
    {
      protected:
        void SetUp() override
        {
            reset();
        }

        bool on_wall() const
        {
            return GetParam().clock != Clock::MONOTONIC;
        }

        bool is_cond() const
        {
            return GetParam().wait == Wait::COND;
        }

        // The deadline `ns` from now on the case's clock.
        timespec in(uint64_t ns) const
        {
            if (on_wall())
            {
                return at(wall_ns() + ns);
            }
            return at(g_mono_ns + ns);
        }

        int wait_until(timespec const& abs)
        {
            return ::wait_until(GetParam(), abs);
        }

        // The one kernel wait toward `abs`: its timeout on the monotonic clock, and on the
        // realtime one the deadline itself, as the kernel's wall clock reads it.
        void expect_one_wait_toward(timespec const& abs, uint32_t timeout_us)
        {
            EXPECT_EQ(wait_until(abs), 0);
            EXPECT_EQ(g_calls, 1u);
            EXPECT_EQ(g_handed_on_wall, on_wall()) << "the wait went to the other clock's call";
            if (on_wall())
            {
                uint64_t want = UINT64_MAX;
                if (abs.tv_sec != std::numeric_limits<time_t>::max())
                {
                    want = static_cast<uint64_t>(abs.tv_sec) * 1000000000u
                           + static_cast<uint64_t>(abs.tv_nsec);
                }
                EXPECT_EQ(g_handed_wall, want) << "the kernel was not handed the deadline";
            }
            else
            {
                EXPECT_EQ(g_handed_us, timeout_us);
            }
        }
    };
}

TEST_P(GthrTimed, a_far_deadline_is_one_wait_of_the_longest_its_clock_takes)
{
    expect_one_wait_toward({std::numeric_limits<time_t>::max(), 0}, CEILING_US);
}

TEST_P(GthrTimed, a_deadline_one_nanosecond_away_waits_one_microsecond_or_is_handed_whole)
{
    expect_one_wait_toward(in(1), 1u);
}

TEST_P(GthrTimed, an_exact_microsecond_is_not_rounded_up)
{
    expect_one_wait_toward(in(5005000), 5005u);
}

TEST_P(GthrTimed, a_passed_deadline_is_one_try)
{
    timespec abs = at(wall_ns() - 1000000000u);
    if (not on_wall())
    {
        abs = at(g_mono_ns - 1000000000u);
    }
    g_script.push_back({-KOS_ETIMEDOUT, false, 0});

    EXPECT_EQ(wait_until(abs), ETIMEDOUT);
    if (is_cond())
    {
        EXPECT_EQ(g_calls, 0u);
    }
    else
    {
        EXPECT_EQ(g_calls, 1u);
        EXPECT_EQ(g_handed_on_wall, on_wall());
        if (not on_wall())
        {
            EXPECT_EQ(g_handed_us, 0u);
        }
    }
}

TEST_P(GthrTimed, a_nanosecond_field_out_of_range_is_refused)
{
    EXPECT_EQ(wait_until({101, 1000000000}), EINVAL);
    EXPECT_EQ(wait_until({101, -1}), EINVAL);
    EXPECT_EQ(g_calls, 0u);
}

// Only a monotonic wait has a ceiling; a realtime one is a single kernel wait, however far.
TEST_P(GthrTimed, a_monotonic_wait_that_ends_at_its_ceiling_asks_again_or_returns_spuriously)
{
    uint64_t away_ns = 2u * static_cast<uint64_t>(CEILING_US) * 1000u;
    timespec const abs = in(away_ns);
    g_script.push_back({-KOS_ETIMEDOUT, true, 0});
    g_script.push_back({-KOS_ETIMEDOUT, true, 0});

    int const rc = wait_until(abs);

    if (on_wall())
    {
        EXPECT_EQ(rc, ETIMEDOUT);
        EXPECT_EQ(g_calls, 1u) << "a realtime wait was cut short of its deadline";
    }
    else if (is_cond())
    {
        EXPECT_EQ(rc, 0) << "a capped wait returns as a spurious wake";
        EXPECT_EQ(g_calls, 1u);
    }
    else
    {
        EXPECT_EQ(rc, ETIMEDOUT);
        EXPECT_EQ(g_calls, 2u);
        EXPECT_EQ(g_handed_us, CEILING_US);
    }
}

TEST_P(GthrTimed, a_timeout_at_the_deadline_answers_timed_out)
{
    g_script.push_back({-KOS_ETIMEDOUT, true, 0});

    EXPECT_EQ(wait_until(in(50000000)), ETIMEDOUT);
    EXPECT_EQ(g_calls, 1u);
    if (not on_wall())
    {
        EXPECT_EQ(g_handed_us, 50000u);
    }
}

// The kernel follows a set of the wall clock for a realtime wait, so the library asks once and
// takes its answer; a monotonic wait is not moved by one.
TEST_P(GthrTimed, a_wall_clock_set_back_during_the_wait_moves_only_a_realtime_deadline)
{
    timespec const abs = in(1000000000u);
    // Back a minute, which the reset's wall clock holds above the epoch.
    g_script.push_back({-KOS_ETIMEDOUT, true, -60});
    uint64_t const mono_from = g_mono_ns;

    int const rc = wait_until(abs);

    EXPECT_EQ(rc, ETIMEDOUT);
    EXPECT_EQ(g_calls, 1u);
    if (on_wall())
    {
        EXPECT_EQ(g_mono_ns - mono_from, 61000000000u)
            << "the realtime wait did not last until the set wall clock reached it";
    }
    else
    {
        EXPECT_EQ(g_mono_ns - mono_from, 1000000000u) << "a monotonic wait moved with the set";
    }
}

TEST_P(GthrTimed, a_wall_clock_set_forward_during_the_wait_moves_only_a_realtime_deadline)
{
    // Past the ceiling on the monotonic clock, so that it takes two kernel waits.
    uint64_t away_ns = 10000000000u;
    if (not on_wall())
    {
        away_ns = 2u * static_cast<uint64_t>(CEILING_US) * 1000u;
    }
    timespec const abs = in(away_ns);
    g_script.push_back({-KOS_ETIMEDOUT, true, 86400});
    uint64_t const mono_from = g_mono_ns;

    int const rc = wait_until(abs);

    if (on_wall())
    {
        EXPECT_EQ(rc, ETIMEDOUT);
        EXPECT_EQ(g_calls, 1u);
        EXPECT_EQ(g_mono_ns - mono_from, 0u) << "a realtime wait outlived the set past its deadline";
    }
    else if (is_cond())
    {
        EXPECT_EQ(rc, 0) << "a monotonic wait ended at the set";
        EXPECT_EQ(g_calls, 1u);
    }
    else
    {
        EXPECT_EQ(rc, 0) << "a monotonic lock ended at the set";
        EXPECT_EQ(g_calls, 2u);
    }
}

TEST(GthrClock, a_clock_with_no_wait_is_refused)
{
    reset();
    timespec const abs = {101, 0};
    Mutex m = {};
    RecursiveMutex r = {};
    Cond cv = {};
    clockid_t const other = CLOCK_PROCESS_CPUTIME_ID;

    EXPECT_EQ(__kickos_gthr_mutex_clocklock(&m, other, &abs), EINVAL);
    EXPECT_EQ(__kickos_gthr_recursive_mutex_clocklock(&r, other, &abs), EINVAL);
    EXPECT_EQ(__kickos_gthr_cond_clockwait(&cv, &m, other, &abs), EINVAL);
    EXPECT_EQ(g_calls, 0u);
}

TEST(GthrTimedLock, a_wait_a_kill_cancels_asks_again_at_the_kernel_entry)
{
    reset();
    g_script.push_back({-KOS_ECANCELED, false, 0});
    g_script.push_back({0, false, 0});
    Mutex m = {};
    timespec const abs = {101, 0};

    EXPECT_EQ(__kickos_gthr_mutex_timedlock(&m, &abs), 0);
    EXPECT_EQ(g_calls, 2u);
    EXPECT_TRUE(g_handed_on_wall) << "the second ask left the wall clock";
    EXPECT_EQ(g_handed_wall, 101000000000u);
}

TEST(GthrSelf, root_has_a_thread_id_and_others_their_handle)
{
    g_self = 0u;
    uint32_t const root = __kickos_gthr_self();
    int const rc = __kickos_gthr_join(root, nullptr);
    g_self = 7u;
    uint32_t const other = __kickos_gthr_self();
    g_self = 1u;

    EXPECT_NE(root, 0u) << "0 is the threads model's no thread";
    EXPECT_EQ(rc, EDEADLK) << "root joining its own id";
    EXPECT_EQ(other, 7u);
}

INSTANTIATE_TEST_SUITE_P(
    Deadline, GthrTimed,
    ::testing::Values(Case{Wait::MUTEX, Clock::TIMED}, Case{Wait::MUTEX, Clock::MONOTONIC},
                      Case{Wait::MUTEX, Clock::REALTIME}, Case{Wait::RECURSIVE, Clock::TIMED},
                      Case{Wait::RECURSIVE, Clock::MONOTONIC},
                      Case{Wait::RECURSIVE, Clock::REALTIME}, Case{Wait::COND, Clock::TIMED},
                      Case{Wait::COND, Clock::MONOTONIC}, Case{Wait::COND, Clock::REALTIME}),
    [](::testing::TestParamInfo<Case> const& p)
    {
        std::string name = "cond";
        if (p.param.wait == Wait::MUTEX)
        {
            name = "mutex";
        }
        else if (p.param.wait == Wait::RECURSIVE)
        {
            name = "recursive";
        }
        if (p.param.clock == Clock::TIMED)
        {
            return name + "_timed";
        }
        if (p.param.clock == Clock::MONOTONIC)
        {
            return name + "_monotonic";
        }
        return name + "_realtime";
    });
