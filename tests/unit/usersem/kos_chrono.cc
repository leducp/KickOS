// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The std::chrono conversions of <kickos/kos.h>: a duration rounds up to the nanosecond and
// saturates, a floating count included, max() is the no-limit value, and each clock's point
// converts to its own kos type and no other. kos::after reads a scripted clock.

#include <gtest/gtest.h>

#include <chrono>
#include <limits>
#include <ratio>
#include <type_traits>

#include <kickos/kos.h>

namespace
{
    int64_t g_now_ns = 0;
}

extern "C" int64_t kickos_monotonic_now(void)
{
    return g_now_ns;
}

namespace
{
    using std::chrono::duration;
    using std::chrono::hours;
    using std::chrono::microseconds;
    using std::chrono::milliseconds;
    using std::chrono::nanoseconds;
    using std::chrono::steady_clock;
    using std::chrono::system_clock;
    using picoseconds = duration<int64_t, std::pico>;

    // Sub-nanosecond integer reps round toward plus infinity.
    static_assert(kos::detail::ns_ceil(picoseconds{1}) == 1);
    static_assert(kos::detail::ns_ceil(picoseconds{1000}) == 1);
    static_assert(kos::detail::ns_ceil(picoseconds{1001}) == 2);
    static_assert(kos::detail::ns_ceil(picoseconds{-1}) == 0);
    static_assert(kos::detail::ns_ceil(picoseconds{-1001}) == -1);

    // Coarser reps scale exactly, negatives included, and saturate at the int64_t range.
    static_assert(kos::to_kos(milliseconds{3}).ns == 3000000);
    static_assert(kos::to_kos(milliseconds{-3}).ns == -3000000);
    static_assert(kos::to_kos(hours{3000000}).ns == INT64_MAX);
    static_assert(kos::to_kos(hours{-3000000}).ns == INT64_MIN);
    static_assert(kos::to_kos(duration<uint64_t, std::nano>{UINT64_MAX}).ns == INT64_MAX);

    // An unsigned count past the int64_t range scales before it saturates, in every period class.
    using upicoseconds = duration<uint64_t, std::pico>;
    using uhalfnanoseconds = duration<uint64_t, std::ratio<1, 2000000000>>;
    using umicroseconds = duration<uint64_t, std::micro>;
    using uthirds = duration<uint64_t, std::ratio<1, 3>>;
    static_assert(kos::to_kos(upicoseconds{UINT64_MAX - 1}).ns == 18446744073709552);
    static_assert(kos::detail::ns_ceil(upicoseconds{UINT64_MAX}) == 18446744073709552);
    static_assert(kos::to_kos(uhalfnanoseconds{UINT64_MAX - 4}).ns == INT64_MAX - 1);
    static_assert(kos::to_kos(uhalfnanoseconds{UINT64_MAX - 2}).ns == INT64_MAX);
    static_assert(kos::detail::ns_ceil(uhalfnanoseconds{UINT64_MAX}) == INT64_MAX);
    static_assert(kos::to_kos(umicroseconds{9223372036854775}).ns == 9223372036854775000);
    static_assert(kos::to_kos(umicroseconds{9223372036854776}).ns == INT64_MAX);
    static_assert(kos::to_kos(umicroseconds{UINT64_MAX - 1}).ns == INT64_MAX);
    static_assert(kos::to_kos(uthirds{1}).ns == 333333334);
    static_assert(kos::to_kos(uthirds{27670116110}).ns == 9223372036666666667);
    static_assert(kos::to_kos(uthirds{27670116111}).ns == INT64_MAX);
    static_assert(kos::to_kos(duration<uint32_t, std::pico>{UINT32_MAX - 1}).ns == 4294968);
    static_assert(kos::to_kos(duration<int8_t, std::milli>{-128}).ns == -128000000);

    // max() is the no-limit value of the type it converts to.
    static_assert(kos::to_kos(nanoseconds::max()).ns == KOS_FOREVER.ns);
    static_assert(kos::to_kos(std::chrono::seconds::max()).ns == KOS_FOREVER.ns);
    static_assert(kos::to_kos(steady_clock::time_point::max()).ns == KOS_NEVER.ns);
    static_assert(kos::to_kos(std::chrono::time_point<steady_clock, hours>::max()).ns
                  == KOS_NEVER.ns);
    static_assert(kos::to_kos(duration<int32_t, std::milli>::max()).ns == KOS_FOREVER.ns);
    static_assert(kos::to_kos(duration<int32_t>::max()).ns == KOS_FOREVER.ns);
    static_assert(kos::to_kos(picoseconds::max()).ns == KOS_FOREVER.ns);
    static_assert(kos::to_kos(upicoseconds::max()).ns == KOS_FOREVER.ns);
    static_assert(kos::to_kos(duration<double, std::pico>::max()).ns == KOS_FOREVER.ns);
    template <class Clock>
    constexpr bool max_points_never()
    {
        using std::chrono::time_point;
        return kos::to_kos(time_point<Clock, duration<int32_t, std::milli>>::max()).ns
                   == KOS_NEVER.ns
               and kos::to_kos(time_point<Clock, picoseconds>::max()).ns == KOS_NEVER.ns
               and kos::to_kos(time_point<Clock, upicoseconds>::max()).ns == KOS_NEVER.ns
               and kos::to_kos(time_point<Clock, duration<double>>::max()).ns == KOS_NEVER.ns;
    }
    static_assert(max_points_never<steady_clock>());
    static_assert(max_points_never<system_clock>());

    // min() is no special value: it converts to zero or less, which a wait or a sleep polls.
    static_assert(kos::to_kos(duration<int32_t, std::milli>::min()).ns == -2147483648000000);
    static_assert(kos::to_kos(picoseconds::min()).ns == -9223372036854775);
    static_assert(kos::to_kos(duration<int64_t, std::femto>::min()).ns == -9223372036854);
    static_assert(kos::to_kos(upicoseconds::min()).ns == 0);
    static_assert(kos::to_kos(duration<double>::min()).ns == INT64_MIN);
    static_assert(kos::to_kos(steady_clock::time_point::min()).ns == INT64_MIN);
    static_assert(kos::to_kos(system_clock::time_point::min()).ns == INT64_MIN);

    // Floating reps round up too, and a NaN is 0.
    static_assert(kos::detail::ns_ceil(duration<double, std::micro>{1.0001}) == 1001);
    static_assert(kos::detail::ns_ceil(duration<double, std::micro>{-1.0001}) == -1000);
    static_assert(kos::detail::ns_ceil(duration<double, std::nano>{0.5}) == 1);
    static_assert(kos::detail::ns_ceil(duration<double>{1e18}) == INT64_MAX);
    static_assert(kos::detail::ns_ceil(duration<double>{-1e18}) == INT64_MIN);
    static_assert(
        kos::detail::ns_ceil(duration<double>{std::numeric_limits<double>::quiet_NaN()}) == 0);

    // Each clock's point converts to its own kos type, and back.
    static_assert(
        std::is_same_v<decltype(kos::to_kos(steady_clock::time_point{})), kos_monotonic_t>);
    static_assert(
        std::is_same_v<decltype(kos::to_kos(system_clock::time_point{})), kos_realtime_t>);
    static_assert(std::is_same_v<decltype(kos::to_kos(nanoseconds{})), kos_duration_t>);
    static_assert(kos::to_kos(steady_clock::time_point{milliseconds{7}}).ns == 7000000);
    static_assert(kos::to_kos(system_clock::time_point{microseconds{-5}}).ns == -5000);
    static_assert(kos::to_chrono(kos_duration_t{-42}) == nanoseconds{-42});
    static_assert(kos::to_chrono(kos_monotonic_t{9}).time_since_epoch() == nanoseconds{9});
    static_assert(kos::to_chrono(kos_realtime_t{8000}).time_since_epoch() == microseconds{8});

    // No mix has an overload: a system_clock point is no monotonic deadline, and neither kos
    // instant converts to the other.
    constexpr auto sleeps_until = [](auto t) -> decltype(kos::sleep_until(t)) {};
    constexpr auto notify_waits_until = [](auto t) -> decltype(kos::notify_wait_until(1u, t)) {
        return 0;
    };
    static_assert(std::is_invocable_v<decltype(sleeps_until), steady_clock::time_point>);
    static_assert(not std::is_invocable_v<decltype(sleeps_until), system_clock::time_point>);
    static_assert(std::is_invocable_v<decltype(notify_waits_until), steady_clock::time_point>);
    static_assert(not std::is_invocable_v<decltype(notify_waits_until), system_clock::time_point>);
    static_assert(not std::is_convertible_v<system_clock::time_point, kos_monotonic_t>);
    static_assert(not std::is_convertible_v<kos_realtime_t, kos_monotonic_t>);
    static_assert(not std::is_convertible_v<kos_monotonic_t, kos_realtime_t>);
    static_assert(not std::is_convertible_v<kos_duration_t, kos_monotonic_t>);

    TEST(KosChrono, a_duration_rounds_up_and_saturates_at_run_time_as_at_compile_time)
    {
        picoseconds const one_ps{1};
        picoseconds const minus_one_ps{-1};
        duration<double, std::micro> const frac{1.0001};
        EXPECT_EQ(kos::to_kos(one_ps).ns, 1);
        EXPECT_EQ(kos::to_kos(minus_one_ps).ns, 0);
        EXPECT_EQ(kos::to_kos(frac).ns, 1001);
        EXPECT_EQ(kos::to_kos(hours{3000000}).ns, INT64_MAX);
        EXPECT_EQ(kos::to_kos(duration<double>{std::numeric_limits<double>::quiet_NaN()}).ns, 0);
        EXPECT_EQ(kos::to_kos(steady_clock::time_point::max()).ns, KOS_NEVER.ns);
        EXPECT_EQ(kos::to_kos(system_clock::time_point::max()).ns, INT64_MAX);
        upicoseconds const near_top{UINT64_MAX - 1};
        EXPECT_EQ(kos::to_kos(near_top).ns, 18446744073709552);
        EXPECT_EQ(kos::to_kos(duration<int32_t, std::milli>::max()).ns, KOS_FOREVER.ns);
        EXPECT_EQ(kos::to_kos(picoseconds::max()).ns, KOS_FOREVER.ns);
    }

    TEST(KosChrono, after_is_the_clock_plus_the_duration_saturating)
    {
        g_now_ns = 5000000;
        EXPECT_EQ(kos::after(milliseconds{1}).ns, 6000000);
        EXPECT_EQ(kos::after(picoseconds{1}).ns, 5000001) << "a sub-nanosecond wait rounded down";
        EXPECT_EQ(kos::after(milliseconds{-10}).ns, -5000000) << "a negative wait is a passed one";
        EXPECT_EQ(kos::after(nanoseconds::max()).ns, KOS_NEVER.ns);
        EXPECT_EQ(kos::after(hours{3000000}).ns, KOS_NEVER.ns);
        EXPECT_EQ(kos::after(duration<int32_t, std::milli>::max()).ns, KOS_NEVER.ns);
        EXPECT_LT(kos::after(milliseconds::min()).ns, 0) << "min() is a passed deadline";
        EXPECT_EQ(kos::now().time_since_epoch(), nanoseconds{5000000});
        g_now_ns = 0;
    }
}
