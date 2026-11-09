// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// arch_wrap_extend over a simulated 32-bit counter, its overflow latch and one core's interrupt
// mask. A wrap, the overflow interrupt and a reader of higher priority are delivered at each
// point between the device accesses of a read, and every read must answer a time between the
// true time at its start and at its end.

#include <kickos/arch/wrap_extend.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <vector>

namespace
{
    enum class Point
    {
        BEFORE_COUNT,  // between the first high load and the counter read
        AFTER_COUNT,   // between the counter read and the latch read
        AFTER_PENDING, // between the latch read and the second high load
        SAVE,          // on_overflow, before its mask
        IN_TAKE,       // on_overflow, after the latch clears and before the word moves
    };

    constexpr uint64_t WRAP = uint64_t{1} << 32;

    struct Sim
    {
        uint64_t t = 0;
        bool latch = false;
        bool masked = false;
        bool isr_active = false;
        bool nested_owed = false;
        bool garbage_once = false;
        uint32_t garbage = 0;
        std::function<void(Point)> hook;
    };

    Sim g_sim;

    void advance(uint64_t d)
    {
        uint64_t const before = g_sim.t;
        g_sim.t = g_sim.t + d;
        if ((before >> 32) != (g_sim.t >> 32))
        {
            g_sim.latch = true;
        }
    }

    void at(Point p)
    {
        if (g_sim.hook)
        {
            g_sim.hook(p);
        }
    }

    struct sim_ops
    {
        static uint32_t count()
        {
            at(Point::BEFORE_COUNT);
            uint32_t v = static_cast<uint32_t>(g_sim.t);
            if (g_sim.garbage_once)
            {
                g_sim.garbage_once = false;
                v = g_sim.garbage;
            }
            at(Point::AFTER_COUNT);
            return v;
        }

        static bool pending()
        {
            bool const v = g_sim.latch;
            at(Point::AFTER_PENDING);
            return v;
        }

        static bool take()
        {
            bool const was = g_sim.latch;
            g_sim.latch = false;
            at(Point::IN_TAKE);
            return was;
        }
    };

    std::unique_ptr<kickos::arch_wrap_extend<sim_ops>> g_wrap;

    struct Reading
    {
        uint64_t value;
        uint64_t from;
        uint64_t to;
        bool ok() const { return value >= from and value <= to; }
    };

    Reading read_checked()
    {
        Reading r{};
        r.from = g_sim.t;
        r.value = g_wrap->read();
        r.to = g_sim.t;
        return r;
    }

    std::vector<Reading> g_nested;

    void run_nested_reader()
    {
        std::function<void(Point)> const saved = g_sim.hook;
        g_sim.hook = nullptr;
        g_nested.push_back(read_checked());
        g_sim.hook = saved;
    }

    void deliver_isr()
    {
        if (g_sim.masked or g_sim.isr_active or not g_sim.latch)
        {
            return;
        }
        g_sim.isr_active = true;
        g_wrap->on_overflow();
        g_sim.isr_active = false;
    }

    // A reader of higher priority than the overflow interrupt, raised now: masked, it runs as
    // the mask lifts.
    void raise_nested_reader()
    {
        if (g_sim.masked)
        {
            g_sim.nested_owed = true;
            return;
        }
        run_nested_reader();
    }

    void unmasked()
    {
        if (g_sim.nested_owed)
        {
            g_sim.nested_owed = false;
            run_nested_reader();
        }
        deliver_isr();
    }

    class WrapExtend : public ::testing::Test
    {
    protected:
        void SetUp() override { reset(); }

        void reset()
        {
            g_sim = Sim{};
            g_nested.clear();
            g_wrap = std::make_unique<kickos::arch_wrap_extend<sim_ops>>();
        }

        // Puts the clock at `t` with the overflow interrupt caught up.
        void start_at(uint64_t t)
        {
            uint64_t const wraps = t >> 32;
            for (uint64_t i = 0; i < wraps; i++)
            {
                g_sim.latch = true;
                g_wrap->on_overflow();
            }
            g_sim.t = t;
            g_sim.latch = false;
        }

        // One action at the first time a read passes `p`.
        void once_at(Point p, std::function<void()> action)
        {
            auto fired = std::make_shared<bool>(false);
            g_sim.hook = [p, action, fired](Point q) {
                if (q == p and not *fired)
                {
                    *fired = true;
                    action();
                }
            };
        }
    };

    struct mask_guard
    {
        mask_guard() { s = arch_irq_save(); }
        ~mask_guard() { arch_irq_restore(s); }
        arch_irq_state_t s;
    };

    constexpr Point READ_POINTS[] = {Point::BEFORE_COUNT, Point::AFTER_COUNT, Point::AFTER_PENDING};
}

arch_irq_state_t arch_irq_save(void)
{
    at(Point::SAVE);
    arch_irq_state_t const before = g_sim.masked;
    g_sim.masked = true;
    return before;
}

void arch_irq_restore(arch_irq_state_t state)
{
    g_sim.masked = state != 0;
    if (not g_sim.masked)
    {
        unmasked();
    }
}

// Interrupts masked across the read, so the overflow interrupt waits: the latch alone tells
// the reader the counter wrapped, wherever in the read the wrap lands.
TEST_F(WrapExtend, AWrapWhileMaskedIsCountedWhereverItLands)
{
    for (Point const p : READ_POINTS)
    {
        reset();
        start_at(WRAP - 1);
        once_at(p, [] { advance(2); });
        Reading r{};
        {
            mask_guard m;
            r = read_checked();
        }
        EXPECT_TRUE(r.ok()) << "wrap at point " << static_cast<int>(p) << ": " << r.value
                            << " outside [" << r.from << ", " << r.to << "]";
        g_sim.hook = nullptr;
        Reading const after = read_checked();
        EXPECT_EQ(after.value, WRAP + 1);
    }
}

// The overflow interrupt preempts an unmasked reader between its accesses.
TEST_F(WrapExtend, AReaderPreemptedByTheOverflowInterruptRereads)
{
    for (Point const p : READ_POINTS)
    {
        reset();
        start_at(WRAP - 1);
        once_at(p, [] {
            advance(2);
            deliver_isr();
        });
        Reading const r = read_checked();
        EXPECT_TRUE(r.ok()) << "preempted at point " << static_cast<int>(p) << ": " << r.value
                            << " outside [" << r.from << ", " << r.to << "]";
    }
}

// A reader above the overflow interrupt's priority, raised while that interrupt runs: before
// its mask it must see the latch, inside it it must wait.
TEST_F(WrapExtend, AHigherPriorityReaderNeverSeesHalfAnAdvance)
{
    for (Point const p : {Point::SAVE, Point::IN_TAKE})
    {
        reset();
        start_at(WRAP - 2);
        advance(6);
        once_at(p, [] { raise_nested_reader(); });
        deliver_isr();
        ASSERT_EQ(g_nested.size(), 1u) << "point " << static_cast<int>(p);
        EXPECT_TRUE(g_nested[0].ok()) << "raised at point " << static_cast<int>(p) << ": "
                                      << g_nested[0].value << " outside [" << g_nested[0].from
                                      << ", " << g_nested[0].to << "]";
        g_sim.hook = nullptr;
        EXPECT_EQ(read_checked().value, WRAP + 4);
    }
}

TEST_F(WrapExtend, AnEntryWithTheLatchClearMovesNothing)
{
    start_at(WRAP + 7);
    g_sim.isr_active = true;
    g_wrap->on_overflow();
    g_sim.isr_active = false;
    EXPECT_EQ(read_checked().value, WRAP + 7);
}

// One wild counter reading costs one wrong answer, never a lasting jump.
TEST_F(WrapExtend, ABadReadingIsNotLatched)
{
    start_at(0x10);
    g_sim.garbage_once = true;
    g_sim.garbage = 0xFFFFFFF0u;
    (void)read_checked();
    Reading const r = read_checked();
    EXPECT_EQ(r.value, 0x10u);
}

// The limit: an overflow interrupt kept waiting half a wrap period or more answers one wrap
// behind, so the high word is still 0 where the sum has carried into it.
TEST_F(WrapExtend, TheOverflowInterruptMayWaitUnderHalfAPeriod)
{
    start_at(WRAP - 1);
    mask_guard m;
    advance(1u + 0x7FFFFFFFu);
    EXPECT_EQ(read_checked().value, WRAP + 0x7FFFFFFFu);
    advance(1);
    EXPECT_EQ(read_checked().value, 0x80000000u);
}

// Random schedules: wraps, overflow interrupts and higher-priority readers at every point of
// reads made masked and unmasked; each answer lies inside its read and none goes backwards.
TEST_F(WrapExtend, RandomSchedulesStayInsideEveryRead)
{
    std::mt19937_64 rng(0x5EEDu);
    for (int round = 0; round < 2000; round++)
    {
        reset();
        start_at((uint64_t{rng() % 4u} << 32) + WRAP - 1 - (rng() % 64u));
        g_sim.hook = [&rng](Point q) {
            if (q == Point::IN_TAKE)
            {
                if (rng() % 4u == 0u)
                {
                    raise_nested_reader();
                }
                return;
            }
            if (q != Point::SAVE)
            {
                advance(rng() % 24u);
            }
            if (rng() % 3u == 0u)
            {
                deliver_isr();
            }
            if (rng() % 5u == 0u)
            {
                raise_nested_reader();
            }
        };
        uint64_t last = 0;
        for (int i = 0; i < 64; i++)
        {
            Reading r{};
            if (rng() % 2u == 0u)
            {
                mask_guard m;
                r = read_checked();
            }
            else
            {
                r = read_checked();
                deliver_isr();
            }
            ASSERT_TRUE(r.ok()) << "round " << round << " read " << i << ": " << r.value
                                << " outside [" << r.from << ", " << r.to << "]";
            ASSERT_GE(r.value, last) << "round " << round << " read " << i;
            last = r.value;
        }
        for (Reading const& n : g_nested)
        {
            ASSERT_TRUE(n.ok()) << "round " << round << ": nested " << n.value << " outside ["
                                << n.from << ", " << n.to << "]";
        }
        g_sim.hook = nullptr;
    }
}
