// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The distribution family's bucketing and its percentile walk. Pure arithmetic over a caller's
// storage, so the host gate exercises the SHIPPED bodies (tests/unit/benchdist).

#ifndef KICKOS_BENCH_HIST_H
#define KICKOS_BENCH_HIST_H

#include <stdint.h>

// always_inline is load-bearing, not a hint: with two callers gcc stops inlining bench_bucket
// at -Os, and the CALL is what gives the switch tail and every accumulator a frame.
#define KICKOS_BENCH_HIST_INLINE inline __attribute__((always_inline))

namespace kickos
{
    // Log-linear buckets: the three bits under the value's most significant one, so a bucket
    // spans at most an eighth of its own octave. A reported percentile is its bucket's LOW edge
    // and therefore a FLOOR, and two runs are comparable only to that resolution. 168 buckets
    // reach 2^23; a bigger delta saturates the last one.
    constexpr uint32_t BENCH_HIST = 168;

    // No __builtin_clz: rv32imac has no clz instruction, so it would resolve to a libgcc call,
    // and this runs from a trap-path root the stack-descent gate measures.
    KICKOS_BENCH_HIST_INLINE constexpr uint32_t bench_bucket(uint32_t v)
    {
        if (v < 8u)
        {
            return v;
        }
        uint32_t e = 3u;
        // THE BOUND IS NOT DECORATION: without it a delta at or above 2^31 shifts by 32, which
        // is undefined and which both backends here answer by masking the count to zero, so the
        // test never fails and the accumulator spins inside a masked window forever. A wrapped
        // counter or a stamp taken before a reset produces exactly such a delta.
        while (e < 31u and (v >> (e + 1u)) != 0u)
        {
            e++;
        }
        uint32_t const idx = 8u + (e - 3u) * 8u + ((v >> (e - 3u)) & 7u);
        if (idx >= BENCH_HIST)
        {
            return BENCH_HIST - 1u;
        }
        return idx;
    }

    KICKOS_BENCH_HIST_INLINE constexpr uint32_t bench_bucket_low(uint32_t idx)
    {
        if (idx < 8u)
        {
            return idx;
        }
        uint32_t const e = 3u + (idx - 8u) / 8u;
        return (8u + ((idx - 8u) % 8u)) << (e - 3u);
    }

    constexpr bool bench_hist_sane()
    {
        uint32_t prev = 0;
        for (uint32_t v = 0; v < 200000u; v += 13u)
        {
            uint32_t const b = bench_bucket(v);
            if (b < prev)
            {
                return false;
            }
            prev = b;
            uint32_t const lo = bench_bucket_low(b);
            if (lo > v)
            {
                return false;
            }
            if ((v - lo) > ((lo / 8u) + 1u))
            {
                return false;
            }
        }
        return true;
    }
    static_assert(bench_hist_sane(), "a percentile bucket must contain its own sample");
    static_assert(bench_bucket(1u << 23) == BENCH_HIST - 1u,
                  "a delta past the last bucket edge saturates rather than wrapping");
    // These two are also the control on the exponent bound above: an unbounded loop makes the
    // constant expression non-constant, so the regression is a build failure and not a hang.
    static_assert(bench_bucket(0x80000000u) == BENCH_HIST - 1u,
                  "a delta at the top of the range saturates rather than spinning");
    static_assert(bench_bucket(0xFFFFFFFFu) == BENCH_HIST - 1u,
                  "a delta at the top of the range saturates rather than spinning");

    // The population every reported percentile is a quantile OF: the family's rows summed
    // BUCKET-WISE into one copy, with the total taken in the SAME pass as the buckets. A board
    // whose workload feeds a row keeps writing it while this runs, so each row is read once and
    // the copy holds the union of every row's samples up to the instant that row was read.
    //
    // `stride` is the distance in uint32_t between one row's bucket array and the next, so the
    // caller hands the first row and never an array of pointers. 672 bytes, so a caller on a
    // chain the stack-descent gate measures keeps this out of its frame.
    //
    // BOUNDED RUN: fewer than 2^32 samples in any ONE bucket of any one distribution. The
    // caller's buckets are uint32_t, so that is where a long enough run wraps. The rep counts
    // that decide how close a run gets carry a static_assert on this bound
    // (user/apps/common/bench/main.cc).
    struct BenchHistSnap
    {
        uint32_t bucket[BENCH_HIST];
        uint32_t total;
    };

    inline void bench_hist_snapshot(uint32_t const* hist, uint32_t stride, uint32_t rows,
                                    BenchHistSnap& out)
    {
        uint32_t total = 0;
        for (uint32_t i = 0; i < BENCH_HIST; i++)
        {
            uint32_t here = 0;
            for (uint32_t r = 0; r < rows; r++)
            {
                here += hist[r * stride + i];
            }
            out.bucket[i] = here;
            total += here;
        }
        out.total = total;
    }

    // The lowest bucket edge at or above the num/den-th sample. THE RANK IS THE SNAPSHOT'S OWN
    // TOTAL: a percentile whose rank comes from one population and whose buckets come from
    // another is a quantile of neither.
    inline uint32_t bench_hist_percentile(BenchHistSnap const& s, uint32_t num, uint32_t den)
    {
        if (s.total == 0)
        {
            return 0;
        }
        uint64_t const target = (static_cast<uint64_t>(s.total) * num + den - 1ull) / den;
        uint64_t seen = 0;
        uint32_t last = 0;
        for (uint32_t i = 0; i < BENCH_HIST; i++)
        {
            uint32_t const here = s.bucket[i];
            if (here != 0)
            {
                last = i;
            }
            seen += here;
            if (seen >= target)
            {
                return bench_bucket_low(i);
            }
        }
        // Unreachable from bench_hist_snapshot, whose total is the sum of these buckets. A
        // hand-built snapshot claiming more samples than the buckets hold still gets a floor.
        return bench_bucket_low(last);
    }
}

#endif
