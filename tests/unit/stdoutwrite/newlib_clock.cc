// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The clocks of user/src/newlib_stubs.cc, through newlib_stubs_shim.cc's renames, over a
// scripted monotonic clock, wall clock, sleep and wall-clock set. The stubs compile against the
// host's <time.h> here, so the clock ids are the host's.

#include <gtest/gtest.h>

#include <errno.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>

#include <kickos/sys.h>
#include <kickos/sys/errno.h>

namespace
{
    constexpr uint64_t NS = 1000000000ull;

    uint64_t g_now = 1;
    uint64_t g_realtime = 0;
    int g_set_rc = 0;
    int g_sets = 0;
    uint64_t g_set_to = 0;
    int g_sleeps = 0;
    int64_t g_slept = 0;
}

extern "C"
{
    int64_t kickos_monotonic_now(void)
    {
        return static_cast<int64_t>(g_now);
    }

    int64_t kickos_realtime_now(void)
    {
        return static_cast<int64_t>(g_realtime);
    }

    int kos_realtime_set(kos_realtime_t t)
    {
        g_sets = g_sets + 1;
        g_set_to = static_cast<uint64_t>(t.ns);
        return g_set_rc;
    }

    void kos_sleep_for(kos_duration_t d)
    {
        g_sleeps = g_sleeps + 1;
        g_slept = d.ns;
    }

    int _gettimeofday(struct timeval* tv, void* tz);
    int kos_ut_clock_gettime(clockid_t id, struct timespec* ts);
    int kos_ut_clock_settime(clockid_t id, struct timespec const* ts);
    int kos_ut_nanosleep(struct timespec const* req, struct timespec* rem);
}

namespace
{
    class NewlibClock : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            g_now = 1;
            g_realtime = 0;
            g_set_rc = 0;
            g_sets = 0;
            g_set_to = 0;
            g_sleeps = 0;
            g_slept = 0;
            errno = 0;
        }
    };

    TEST_F(NewlibClock, EachClockReadsItsOwnSource)
    {
        g_now = 7 * NS + 5;
        g_realtime = 1700000000ull * NS + 999999999ull;
        struct timespec ts = {};
        ASSERT_EQ(kos_ut_clock_gettime(CLOCK_MONOTONIC, &ts), 0);
        EXPECT_EQ(ts.tv_sec, 7);
        EXPECT_EQ(ts.tv_nsec, 5);
        ASSERT_EQ(kos_ut_clock_gettime(CLOCK_REALTIME, &ts), 0);
        EXPECT_EQ(ts.tv_sec, 1700000000);
        EXPECT_EQ(ts.tv_nsec, 999999999);
    }

    TEST_F(NewlibClock, AnUnknownClockIsRefused)
    {
        struct timespec ts = {};
        EXPECT_EQ(kos_ut_clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts), -1);
        EXPECT_EQ(errno, EINVAL);
    }

    TEST_F(NewlibClock, GettimeofdayIsTheWallClockInMicroseconds)
    {
        g_now = 3 * NS;
        g_realtime = 1700000000ull * NS + 123456789ull;
        struct timeval tv = {};
        ASSERT_EQ(_gettimeofday(&tv, nullptr), 0);
        EXPECT_EQ(tv.tv_sec, 1700000000);
        EXPECT_EQ(tv.tv_usec, 123456);
        EXPECT_EQ(_gettimeofday(nullptr, nullptr), 0) << "a null time is no fault";
    }

    TEST_F(NewlibClock, ASetReachesTheKernelInNanoseconds)
    {
        struct timespec const ts = {1700000000, 42};
        ASSERT_EQ(kos_ut_clock_settime(CLOCK_REALTIME, &ts), 0);
        EXPECT_EQ(g_sets, 1);
        EXPECT_EQ(g_set_to, 1700000000ull * NS + 42u);
    }

    TEST_F(NewlibClock, ASetWithoutTheAuthorityIsEperm)
    {
        g_set_rc = -KOS_EPERM;
        struct timespec const ts = {1, 0};
        EXPECT_EQ(kos_ut_clock_settime(CLOCK_REALTIME, &ts), -1);
        EXPECT_EQ(errno, EPERM);
    }

    TEST_F(NewlibClock, TheMonotonicClockIsNeverSet)
    {
        struct timespec const ts = {1, 0};
        EXPECT_EQ(kos_ut_clock_settime(CLOCK_MONOTONIC, &ts), -1);
        EXPECT_EQ(errno, EINVAL);
        EXPECT_EQ(g_sets, 0);
    }

    TEST_F(NewlibClock, AMalformedTimeIsRefusedBeforeTheKernel)
    {
        struct timespec const bad[] = {{-1, 0}, {0, -1}, {0, 1000000000}};
        for (struct timespec const& ts : bad)
        {
            errno = 0;
            EXPECT_EQ(kos_ut_clock_settime(CLOCK_REALTIME, &ts), -1);
            EXPECT_EQ(errno, EINVAL);
            errno = 0;
            EXPECT_EQ(kos_ut_nanosleep(&ts, nullptr), -1);
            EXPECT_EQ(errno, EINVAL);
        }
        EXPECT_EQ(g_sets, 0);
        EXPECT_EQ(g_sleeps, 0);
    }

    TEST_F(NewlibClock, NanosleepSleepsTheWholeRequest)
    {
        struct timespec const req = {2, 500};
        struct timespec rem = {9, 9};
        ASSERT_EQ(kos_ut_nanosleep(&req, &rem), 0);
        EXPECT_EQ(g_sleeps, 1);
        EXPECT_EQ(g_slept, static_cast<int64_t>(2 * NS + 500u));
        EXPECT_EQ(rem.tv_sec, 9) << "an unbroken sleep leaves rem alone";
    }

    TEST_F(NewlibClock, ASleepPastTheClockRangeSaturates)
    {
        struct timespec const req = {INT64_MAX, 999999999};
        ASSERT_EQ(kos_ut_nanosleep(&req, nullptr), 0);
        EXPECT_EQ(g_slept, KOS_FOREVER.ns);
    }
}
