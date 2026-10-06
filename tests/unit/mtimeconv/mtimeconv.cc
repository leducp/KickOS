// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Pins mtime_conv.h: the PCR clock fields to MTIME's rate, and mtime_ns_to_ticks's reciprocal
// multiply against the plain "ns * 4ull / 25ull" it replaces, over the whole domain the multiply
// is proved exact for, at every magnitude class a real uptime passes through.

#include "arch/riscv/chip/esp32c6/mtime_conv.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>

namespace
{
    using kickos::esp32c6::cpu_clk_hz;
    using kickos::esp32c6::mtime_ns_to_ticks;
    using kickos::esp32c6::mtime_shift_of;
    using kickos::esp32c6::mtime_ticks_to_ns;
    using kickos::esp32c6::mtime_umulh64;

    // The bound mtime_conv.h derives and documents: largest N such that
    // (ns*MAGIC)>>66 == floor(4*ns/25) for every ns in [0, N). Duplicated here so a
    // change to the derivation shows up as a diff in two places, not a silent widening.
    constexpr uint64_t EXACT_BOUND = 3883525068149379288ull;

    uint64_t old_ns_to_ticks(uint64_t ns) { return ns * 4ull / 25ull; }

    void expect_pinned(uint64_t ns)
    {
        EXPECT_EQ(mtime_ns_to_ticks(ns, 0u), old_ns_to_ticks(ns)) << "ns=" << ns;
    }
}

TEST(MtimeConv, BoundaryValues)
{
    expect_pinned(0);
    expect_pinned(1);
    expect_pinned(2);
    expect_pinned(3);
    expect_pinned(4);
    expect_pinned(24);
    expect_pinned(25);
    expect_pinned(26);
    expect_pinned(49);
    expect_pinned(50);
    expect_pinned(51);
}

// Bit 32 is where mtime_umulh64 splits both operands into halves: any carry-propagation
// bug in the cross-term addition shows up right around here first.
TEST(MtimeConv, Bit32Crossing)
{
    expect_pinned(0xFFFFFFFFull);
    expect_pinned(0x100000000ull);
    expect_pinned(0x100000001ull);
    expect_pinned(0xFFFFFFFEull);
    expect_pinned(0x80000000ull);
    expect_pinned(0x7FFFFFFFull);
}

TEST(MtimeConv, DenseNearZero)
{
    for (uint64_t ns = 0; ns < 200000ull; ns++)
    {
        expect_pinned(ns);
    }
}

// Exactness holds up to (not including) EXACT_BOUND: dense right up to it, from both
// sides, so a wrong bound (off by even a handful of ns) fails here rather than in the
// field. What happens AT or PAST the bound is deliberately not asserted: the derivation
// is a proven sufficient bound, not a claim that failure starts exactly there.
TEST(MtimeConv, DenseAtBound)
{
    for (uint64_t ns = EXACT_BOUND - 5000ull; ns < EXACT_BOUND; ns++)
    {
        expect_pinned(ns);
    }
}

// One sample per power-of-two magnitude (plus neighbours) from a few seconds of uptime
// out past the proven bound: an uptime-dependent regression is far more likely to show
// up as "wrong at large ns, right at small ns" than as a uniformly-distributed failure,
// so log-spaced coverage matters more here than raw sample count.
TEST(MtimeConv, LogSpacedSweep)
{
    for (int bit = 0; bit <= 61; bit++)
    {
        uint64_t const base = (uint64_t{1} << bit);
        for (uint64_t delta : {uint64_t{0}, uint64_t{1}, uint64_t{7}, uint64_t{12345}})
        {
            uint64_t const ns = base + delta;
            if (ns < EXACT_BOUND)
            {
                expect_pinned(ns);
            }
        }
    }
}

// Uniform-random coverage within the proven-exact domain: not a substitute for the
// log-spaced sweep above (uniform-random over [0, EXACT_BOUND) is dominated by large
// ns, the same way uniform-random over [0, 2**64) would be dominated by ns no real
// uptime reaches), but a cheap extra witness against a bug that only a few random
// bit patterns would trip.
TEST(MtimeConv, RandomSweepWithinBound)
{
    std::mt19937_64 rng(0xE5C6);
    std::uniform_int_distribution<uint64_t> dist(0, EXACT_BOUND - 1);
    for (int i = 0; i < 200000; i++)
    {
        expect_pinned(dist(rng));
    }
}

// A realistic uptime is milliseconds to a handful of years; EXACT_BOUND is about 123
// years. This pins one concrete "a very long-lived deployment" point well inside the
// proven domain, in ns rather than in bit tricks, as a readability anchor for the bound
// above.
TEST(MtimeConv, TenYearUptime)
{
    uint64_t constexpr TEN_YEARS_NS = 10ull * 365ull * 24ull * 3600ull * 1000000000ull;
    static_assert(TEN_YEARS_NS < EXACT_BOUND, "the anchor point must sit inside the proven domain");
    expect_pinned(TEN_YEARS_NS);
}

// mtime_umulh64 (the reciprocal's multiply-high primitive) against an independent
// 128-bit oracle. __int128 is a host-only convenience: rv32 has no such type, which is
// exactly why mtime_umulh64 exists, but the host running this test can use it as ground
// truth without touching the code under test.
TEST(MtimeConv, Umulh64AgainstInt128)
{
    std::mt19937_64 rng(0x0B17);
    std::uniform_int_distribution<uint64_t> dist(0, UINT64_MAX);
    auto check = [](uint64_t a, uint64_t b)
    {
        unsigned __int128 const wide = static_cast<unsigned __int128>(a) * b;
        uint64_t const want = static_cast<uint64_t>(wide >> 64);
        EXPECT_EQ(mtime_umulh64(a, b), want) << "a=" << a << " b=" << b;
    };
    check(0, 0);
    check(UINT64_MAX, UINT64_MAX);
    check(UINT64_MAX, 1);
    check(1, UINT64_MAX);
    check(0x100000000ull, 0x100000000ull);
    for (int i = 0; i < 200000; i++)
    {
        check(dist(rng), dist(rng));
    }
}

// Every shift against a 128-bit oracle, both directions.
TEST(MtimeConv, ShiftedAgainstInt128)
{
    std::mt19937_64 rng(0x71C5);
    std::uniform_int_distribution<uint64_t> ns_dist(0, EXACT_BOUND - 1);
    for (uint32_t shift = 0; shift <= kickos::esp32c6::MTIME_SHIFT_MAX; shift++)
    {
        // The ~146 years mtime_ticks_to_ns holds at every shift.
        std::uniform_int_distribution<uint64_t> tick_dist(0, UINT64_MAX / (25u << shift));
        for (int i = 0; i < 20000; i++)
        {
            uint64_t const ns = ns_dist(rng);
            unsigned __int128 const want_ticks = (static_cast<unsigned __int128>(ns) * 4u) / (25u << shift);
            EXPECT_EQ(mtime_ns_to_ticks(ns, shift), static_cast<uint64_t>(want_ticks))
                << "ns=" << ns << " shift=" << shift;
            uint64_t const ticks = tick_dist(rng);
            unsigned __int128 const want_ns = (static_cast<unsigned __int128>(ticks) * (25u << shift)) / 4u;
            EXPECT_EQ(mtime_ticks_to_ns(ticks, shift), static_cast<uint64_t>(want_ns))
                << "ticks=" << ticks << " shift=" << shift;
        }
    }
}

// The reset selection, XTAL / 1, is 40 MHz: 25 ns per tick. 160 MHz would be 6.25.
TEST(MtimeConv, ResetClockIsFortyMegahertz)
{
    uint32_t const hz = cpu_clk_hz(0u, 40u, 0u, 2u, false);
    EXPECT_EQ(hz, 40000000u);
    ASSERT_EQ(mtime_shift_of(hz), 2);
    EXPECT_EQ(mtime_ticks_to_ns(40000000u, 2u), 1000000000u);
    EXPECT_EQ(mtime_ns_to_ticks(1000000000u, 2u), 40000000u);
}

TEST(MtimeConv, ClockTreeReadings)
{
    EXPECT_EQ(mtime_shift_of(cpu_clk_hz(1u, 40u, 0u, 0u, false)), 0);
    EXPECT_EQ(mtime_shift_of(cpu_clk_hz(1u, 40u, 0u, 1u, false)), 1);
    EXPECT_EQ(mtime_shift_of(cpu_clk_hz(1u, 40u, 0u, 3u, false)), 2);
    EXPECT_EQ(mtime_shift_of(cpu_clk_hz(0u, 40u, 1u, 0u, false)), 3);
    EXPECT_EQ(mtime_shift_of(cpu_clk_hz(0u, 40u, 31u, 0u, false)), 7);
    EXPECT_EQ(cpu_clk_hz(1u, 40u, 0u, 0u, true), 120000000u);
    EXPECT_EQ(mtime_shift_of(120000000u), -1);
    EXPECT_EQ(cpu_clk_hz(1u, 40u, 0u, 1u, true), 80000000u);
    EXPECT_EQ(cpu_clk_hz(0u, 40u, 2u, 0u, false), 0u);
    EXPECT_EQ(cpu_clk_hz(2u, 40u, 0u, 0u, false), 0u);
    EXPECT_EQ(cpu_clk_hz(3u, 40u, 0u, 0u, false), 0u);
    EXPECT_EQ(mtime_shift_of(0u), -1);
}
