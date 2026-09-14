// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Host arms for kernel/include/kickos/bench_hist.h.
//
// WHAT THE REPORTED VALUE IS. bench_hist_percentile returns a bucket LOW EDGE, so a reported
// p50 or p99 is a FLOOR and never the quantile itself. The rule it must satisfy is therefore
// two-sided and neither half alone would catch a wrong edge table: the floor is at or below
// the true quantile, and no further below it than an eighth of an octave.
//
// WHICH quantile. The walk takes the lowest bucket whose cumulative count reaches
// ceil(total * num / den), so the sample it names is the nearest-rank one: the ceil(p*n)-th
// smallest, one-indexed. Every arm here computes that rank over a SORTED copy of its own
// samples, so the expected value is derived from the data and never from the bucketing.
//
// WHICH POPULATION. The rank is the SNAPSHOT'S OWN TOTAL, never a count handed in beside the
// buckets, and the last two arms are what says that matters: a report whose rows move under it
// can otherwise pick a rank in one population and read the edge out of another.

#include <kickos/bench_hist.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace
{
    using kickos::BENCH_HIST;
    using kickos::bench_bucket;
    using kickos::bench_bucket_low;
    using kickos::BenchHistSnap;
    using kickos::bench_hist_percentile;
    using kickos::bench_hist_snapshot;

    // One row, the shape a single-core board reports from.
    struct Hist
    {
        uint32_t b[BENCH_HIST] = {};
    };

    // What the kernel does before it asks for a percentile: sum the rows into one copy whose
    // total is the buckets' own.
    BenchHistSnap snap_of(uint32_t const* first, uint32_t stride, uint32_t rows)
    {
        BenchHistSnap s = {};
        bench_hist_snapshot(first, stride, rows, s);
        return s;
    }

    BenchHistSnap snap_of(Hist const& h) { return snap_of(h.b, BENCH_HIST, 1); }

    void feed(Hist& h, std::vector<uint32_t> const& samples)
    {
        for (uint32_t v : samples)
        {
            h.b[bench_bucket(v)]++;
        }
    }

    // The ceil(p*n)-th smallest sample, one-indexed, straight off the sorted data.
    uint32_t nearest_rank(std::vector<uint32_t> samples, uint32_t num, uint32_t den)
    {
        std::sort(samples.begin(), samples.end());
        uint64_t const n = samples.size();
        uint64_t const rank = (n * num + den - 1ull) / den;
        return samples[static_cast<size_t>(rank - 1ull)];
    }

    // The report is a floor within an eighth of an octave of the true quantile. Under 8 the
    // buckets are exact, so the slack there is the +1 the bucket arithmetic already carries.
    void expect_floor_of(uint32_t got, uint32_t want)
    {
        EXPECT_LE(got, want) << "a reported percentile must never exceed its own quantile";
        EXPECT_LE(want - got, (want / 8u) + 1u)
            << "a reported percentile must be within an eighth of an octave below its quantile";
    }

    // A mixture with a long tail: a dense body across three octaves, a decade-out shoulder and
    // a handful of outliers, so p50, p99 and the maximum each land in a different region.
    std::vector<uint32_t> mixture()
    {
        std::vector<uint32_t> v;
        for (uint32_t i = 0; i < 5000u; i++)
        {
            v.push_back(600u + (i % 97u) * 7u);
        }
        for (uint32_t i = 0; i < 4000u; i++)
        {
            v.push_back(3000u + (i % 211u) * 31u);
        }
        for (uint32_t i = 0; i < 900u; i++)
        {
            v.push_back(40000u + (i % 53u) * 811u);
        }
        for (uint32_t i = 0; i < 100u; i++)
        {
            v.push_back(700000u + i * 4001u);
        }
        return v;
    }
}

TEST(BenchHist, every_bucket_edge_is_a_floor_within_an_eighth_of_an_octave)
{
    // EVERY value across the first eleven octaves, which is the range a lock hold and a
    // switch both land in.
    for (uint32_t v = 0; v < 4096u; v++)
    {
        uint32_t const lo = bench_bucket_low(bench_bucket(v));
        ASSERT_LE(lo, v) << "value " << v;
        ASSERT_LE(v - lo, (lo / 8u) + 1u) << "value " << v;
    }
}

TEST(BenchHist, the_edges_are_monotone_and_the_last_one_saturates)
{
    for (uint32_t i = 1; i < BENCH_HIST; i++)
    {
        ASSERT_GE(bench_bucket_low(i), bench_bucket_low(i - 1u)) << "bucket " << i;
    }
    EXPECT_EQ(bench_bucket(1u << 23), BENCH_HIST - 1u);
    EXPECT_EQ(bench_bucket(0xFFFFFFFFu), BENCH_HIST - 1u);
}

TEST(BenchHist, an_empty_accumulator_reports_zero_rather_than_a_bucket)
{
    Hist h;
    EXPECT_EQ(bench_hist_percentile(snap_of(h), 1, 2), 0u);
}

TEST(BenchHist, p50_and_p99_of_a_known_mixture_are_floors_of_its_own_quantiles)
{
    std::vector<uint32_t> const s = mixture();
    Hist h;
    feed(h, s);
    BenchHistSnap const snap = snap_of(h);
    EXPECT_EQ(snap.total, static_cast<uint32_t>(s.size()));

    uint32_t const p50 = bench_hist_percentile(snap, 1, 2);
    uint32_t const p99 = bench_hist_percentile(snap, 99, 100);

    expect_floor_of(p50, nearest_rank(s, 1, 2));
    expect_floor_of(p99, nearest_rank(s, 99, 100));
    // The three regions of the mixture must still be distinguishable after bucketing, or the
    // arms above would pass over a table that collapsed them.
    EXPECT_LT(p50, p99);
    EXPECT_LT(p99, *std::max_element(s.begin(), s.end()));
}

TEST(BenchHist, a_sweep_of_quantiles_over_a_uniform_ramp_each_lands_on_its_own_rank)
{
    std::vector<uint32_t> s;
    for (uint32_t i = 1; i <= 20000u; i++)
    {
        s.push_back(i * 3u);
    }
    Hist h;
    feed(h, s);
    BenchHistSnap const snap = snap_of(h);
    static uint32_t const NUM[] = {1, 1, 1, 1, 1, 9, 19, 49, 99, 999};
    static uint32_t const DEN[] = {100, 20, 10, 4, 2, 10, 20, 50, 100, 1000};
    for (unsigned k = 0; k < sizeof(NUM) / sizeof(NUM[0]); k++)
    {
        uint32_t const got = bench_hist_percentile(snap, NUM[k], DEN[k]);
        expect_floor_of(got, nearest_rank(s, NUM[k], DEN[k]));
    }
}

TEST(BenchHist, a_degenerate_distribution_reports_its_one_value_exactly_where_the_bucket_is)
{
    // Every sample identical: the floor is that value's own bucket edge and nothing else.
    for (uint32_t v : {0u, 1u, 7u, 8u, 9u, 1000u, 65536u, (1u << 23)})
    {
        std::vector<uint32_t> s(500u, v);
        Hist h;
        feed(h, s);
        uint32_t const got = bench_hist_percentile(snap_of(h), 1, 2);
        EXPECT_EQ(got, bench_bucket_low(bench_bucket(v))) << "value " << v;
        expect_floor_of(got, v);
    }
}

TEST(BenchHist, the_walk_sums_the_rows_bucket_wise_rather_than_reading_one)
{
    // Four rows, the arm64 bench posture. Each core sees a DIFFERENT region, so a walk that
    // read row zero alone, or averaged the rows' own percentiles, lands somewhere else.
    std::vector<uint32_t> all;
    Hist rows[4];
    for (uint32_t c = 0; c < 4u; c++)
    {
        std::vector<uint32_t> s;
        for (uint32_t i = 0; i < 1000u; i++)
        {
            s.push_back(100u * (c + 1u) * (c + 1u) + i);
        }
        feed(rows[c], s);
        all.insert(all.end(), s.begin(), s.end());
    }
    uint32_t const stride = sizeof(Hist) / sizeof(uint32_t);
    BenchHistSnap const snap = snap_of(rows[0].b, stride, 4);
    EXPECT_EQ(snap.total, static_cast<uint32_t>(all.size()));
    uint32_t const p50 = bench_hist_percentile(snap, 1, 2);
    uint32_t const p99 = bench_hist_percentile(snap, 99, 100);
    expect_floor_of(p50, nearest_rank(all, 1, 2));
    expect_floor_of(p99, nearest_rank(all, 99, 100));

    // The positive control on the summing itself: row zero alone answers a different number.
    uint32_t const row0 = bench_hist_percentile(snap_of(rows[0].b, stride, 1), 1, 2);
    EXPECT_NE(row0, p50);
}

TEST(BenchHist, a_count_the_histogram_does_not_hold_falls_back_to_the_highest_occupied_edge)
{
    // The reset race: a peer's in-flight sample lands in a row this pass already cleared, so
    // the accumulator counts more than the buckets hold. The answer must still be a floor.
    std::vector<uint32_t> const s = {10u, 20u, 30u, 40u};
    Hist h;
    feed(h, s);
    BenchHistSnap snap = snap_of(h);
    snap.total = 400u; // bench_hist_snapshot cannot produce this; a hand-built one can
    uint32_t const got = bench_hist_percentile(snap, 99, 100);
    EXPECT_EQ(got, bench_bucket_low(bench_bucket(40u)));
    expect_floor_of(got, 40u);
}

namespace
{
    // The board geometry: one row per kernel core, padded to the kernel's stride, each written
    // by its own core and by no other. ATOMIC CELLS, which the kernel's plain uint32_t rows are
    // not: the shipped walk reads them as uint32_t, and std::atomic<uint32_t> is a drop-in for
    // that on every host this gate runs on.
    struct AtomicRow
    {
        std::atomic<uint32_t> cell[BENCH_HIST] = {};
        uint32_t pad[3] = {};
    };
    static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t)
                      and alignof(std::atomic<uint32_t>) == alignof(uint32_t),
                  "the walk reads these cells as plain words");

    // The stop flag lives HERE and not in the arm: an assertion returns out of the test body,
    // and a destructor that joined without first stopping the writers would hang the run rather
    // than report the failure.
    struct Writers
    {
        std::vector<std::thread> t;
        std::atomic<bool> stop{false};
        void halt()
        {
            stop.store(true, std::memory_order_relaxed);
            for (std::thread& w : t)
            {
                if (w.joinable())
                {
                    w.join();
                }
            }
        }
        ~Writers() { halt(); }
    };

    uint32_t const* row_words(AtomicRow const* rows)
    {
        return reinterpret_cast<uint32_t const*>(&rows[0].cell[0]);
    }

    // The nearest-rank rule stated as a two-sided bracket over the snapshot's own buckets: the
    // reported bucket is the FIRST whose cumulative count reaches the rank, so everything below
    // it falls short and it does not.
    void expect_nearest_rank_of(BenchHistSnap const& s, uint32_t num, uint32_t den, uint32_t got)
    {
        if (s.total == 0)
        {
            EXPECT_EQ(got, 0u);
            return;
        }
        uint64_t const target = (static_cast<uint64_t>(s.total) * num + den - 1ull) / den;
        uint64_t below = 0;
        for (uint32_t i = 0; i < BENCH_HIST; i++)
        {
            if (bench_bucket_low(i) == got and below + s.bucket[i] >= target and below < target)
            {
                return;
            }
            below += s.bucket[i];
        }
        ADD_FAILURE() << "reported edge " << got << " is not the " << num << "/" << den
                      << " nearest rank of these " << s.total << " samples";
    }

    // The nearest-rank edge read straight off the snapshot's buckets, one sample at a time and
    // with no cumulative arithmetic of its own, so it shares no code with the walk it checks.
    uint32_t rank_edge_of(BenchHistSnap const& s, uint32_t num, uint32_t den)
    {
        std::vector<uint32_t> idx;
        idx.reserve(s.total);
        for (uint32_t i = 0; i < BENCH_HIST; i++)
        {
            for (uint32_t k = 0; k < s.bucket[i]; k++)
            {
                idx.push_back(i);
            }
        }
        if (idx.empty())
        {
            return 0;
        }
        uint64_t const rank = (static_cast<uint64_t>(idx.size()) * num + den - 1ull) / den;
        return bench_bucket_low(idx[static_cast<size_t>(rank - 1ull)]);
    }
}

TEST(BenchHist, a_snapshot_of_moving_rows_is_one_population_and_the_walk_reports_its_quantiles)
{
    constexpr uint32_t ROWS = 4;
    constexpr uint32_t PASSES = 8;
    // Samples the writers must add before the reporter takes its next snapshot. The wait is on
    // the writers' own counter and not on a delay, so the arm cannot pass over a still table
    // and cannot fail because a box was slow.
    constexpr uint32_t STEP = 2000;

    std::vector<AtomicRow> rows(ROWS);
    uint32_t const stride = sizeof(AtomicRow) / sizeof(uint32_t);
    // ONE CELL PER WRITER and summed by the reporter, which is the kernel's own discipline.
    std::vector<std::atomic<uint32_t>> written(ROWS);

    Writers writers;
    for (uint32_t c = 0; c < ROWS; c++)
    {
        writers.t.emplace_back([&rows, &written, &writers, c]() {
            // Each core its own region, so a snapshot that lost or double-counted a row moves
            // the quantiles rather than blending invisibly into one shape.
            uint32_t i = 0;
            uint32_t mine = 0;
            while (not writers.stop.load(std::memory_order_relaxed))
            {
                uint32_t const v = 100u * (c + 1u) * (c + 1u) + (i % 512u);
                std::atomic<uint32_t>& cell = rows[c].cell[bench_bucket(v)];
                cell.store(cell.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
                mine++;
                written[c].store(mine, std::memory_order_relaxed);
                i++;
            }
        });
    }

    auto total_written = [&written]() {
        uint32_t n = 0;
        for (std::atomic<uint32_t> const& w : written)
        {
            n += w.load(std::memory_order_relaxed);
        }
        return n;
    };

    uint32_t need = ROWS;
    uint32_t moved = 0;
    uint32_t prev = 0;
    for (uint32_t pass = 0; pass < PASSES; pass++)
    {
        while (total_written() < need)
        {
            std::this_thread::yield();
        }
        BenchHistSnap snap = {};
        bench_hist_snapshot(row_words(rows.data()), stride, ROWS, snap);

        uint32_t sum = 0;
        for (uint32_t i = 0; i < BENCH_HIST; i++)
        {
            sum += snap.bucket[i];
        }
        ASSERT_EQ(sum, snap.total)
            << "pass " << pass << ": the rank and the buckets must be one population";

        uint32_t const p50 = bench_hist_percentile(snap, 1, 2);
        uint32_t const p99 = bench_hist_percentile(snap, 99, 100);
        ASSERT_LE(p50, p99) << "pass " << pass;
        expect_nearest_rank_of(snap, 1, 2, p50);
        expect_nearest_rank_of(snap, 99, 100, p99);

        if (snap.total > prev)
        {
            moved++;
        }
        prev = snap.total;
        need = snap.total + STEP;
    }
    writers.halt();

    // WITHOUT THIS THE ARM IS VACUOUS: a reporter that never saw the rows move would assert
    // the same things over a still table.
    EXPECT_EQ(moved, PASSES) << "a snapshot was taken while the rows were not moving";
}

TEST(BenchHist, a_rank_from_another_population_is_a_quantile_of_neither)
{
    // Buckets read now, rank taken from a count read before the workload added to them. Same
    // walk, same buckets, one field of the snapshot stale.
    Hist h;
    std::vector<uint32_t> early;
    for (uint32_t i = 0; i < 1000u; i++)
    {
        early.push_back(200u + i);
    }
    feed(h, early);
    uint32_t const stale = snap_of(h).total;

    std::vector<uint32_t> late;
    for (uint32_t i = 0; i < 1000u; i++)
    {
        late.push_back(40000u + i * 31u);
    }
    feed(h, late);

    BenchHistSnap const honest = snap_of(h);
    EXPECT_EQ(bench_hist_percentile(honest, 99, 100), rank_edge_of(honest, 99, 100));

    BenchHistSnap mixed = honest;
    mixed.total = stale;
    uint32_t const from_stale = bench_hist_percentile(mixed, 99, 100);
    EXPECT_NE(from_stale, rank_edge_of(honest, 99, 100))
        << "a stale rank over these buckets must not answer their own quantile";
    EXPECT_LT(from_stale, bench_hist_percentile(honest, 99, 100))
        << "a rank short of the population reports a percentile that is too low";
}
