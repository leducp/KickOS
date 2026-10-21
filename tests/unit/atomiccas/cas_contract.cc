// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <stdint.h>

#if defined(ATOMICCAS_BOUNDED)
#include <kickos/arch/atomic_cas_bounded.h>

namespace
{
    // LOSE_FOREVER loses every store before attempt UNBOUNDED, so a loop with no bound ends
    // stored instead of hanging.
    constexpr uint32_t LOSE_FOREVER = 0xFFFFFFFFu;
    constexpr uint32_t UNBOUNDED = 1000u;
    uint32_t g_attempts = 0;
    uint32_t g_lose = 0;
    // Written to the word in place of a lost store, as another core's store would be.
    uint32_t g_overwrite = 0;
    bool g_overwrites = false;
}

bool arch_cas_u32_once(uint32_t* word, uint32_t want, uint32_t desired, uint32_t* observed)
{
    g_attempts++;
    *observed = *word;
    if (*observed != want)
    {
        return false;
    }
    if (g_lose != 0 and g_attempts < UNBOUNDED)
    {
        if (g_lose != LOSE_FOREVER)
        {
            g_lose--;
        }
        if (g_overwrites)
        {
            *word = g_overwrite;
        }
        return false;
    }
    *word = desired;
    return true;
}
#elif defined(ATOMICCAS_MASKED)
#include <kickos/arch/atomic_cas_masked.h>
#else
#include <kickos/arch/atomic_cas.h>
#endif

#include <gtest/gtest.h>

#if defined(ATOMICCAS_MASKED)
namespace
{
    arch_irq_state_t g_masked = 0;
    uint32_t g_saves = 0;
    uint32_t g_restores = 0;
    uint32_t* g_watch = nullptr;
    uint32_t g_poke = 0;
    uint32_t g_at_restore = 0;
}

// A nonzero g_poke lands on the watched word as the mask is taken, as a writer that ran just
// before it would.
arch_irq_state_t arch_irq_save(void)
{
    arch_irq_state_t const before = g_masked;
    if (g_watch != nullptr and g_poke != 0u)
    {
        *g_watch = g_poke;
    }
    g_masked = 1;
    g_saves = g_saves + 1u;
    return before;
}

void arch_irq_restore(arch_irq_state_t state)
{
    if (g_watch != nullptr)
    {
        g_at_restore = *g_watch;
    }
    g_restores = g_restores + 1u;
    g_masked = state;
}
#endif

namespace
{
    constexpr uint32_t GUARD = 0xA5A5A5A5u;

    // The neighbours catch a store wider than the word.
    struct Cell
    {
        uint32_t lo;
        uint32_t word;
        uint32_t hi;
    };

    TEST(ArchCasU32, MatchStoresAndLeavesExpected)
    {
        Cell c{GUARD, 5u, GUARD};
        uint32_t expected = 5u;
        EXPECT_TRUE(arch_cas_u32(&c.word, &expected, 9u));
        EXPECT_EQ(c.word, 9u);
        EXPECT_EQ(expected, 5u);
        EXPECT_EQ(c.lo, GUARD);
        EXPECT_EQ(c.hi, GUARD);
    }

    TEST(ArchCasU32, MissWritesNothingAndHandsBackTheWord)
    {
        Cell c{GUARD, 7u, GUARD};
        uint32_t expected = 5u;
        EXPECT_FALSE(arch_cas_u32(&c.word, &expected, 9u));
        EXPECT_EQ(c.word, 7u);
        EXPECT_EQ(expected, 7u);
        EXPECT_EQ(c.lo, GUARD);
        EXPECT_EQ(c.hi, GUARD);
    }

    TEST(ArchCasU32, TopBitIsCompared)
    {
        Cell c{GUARD, 0x80000000u, GUARD};
        uint32_t expected = 0x80000000u;
        EXPECT_TRUE(arch_cas_u32(&c.word, &expected, 0xFFFFFFFFu));
        EXPECT_EQ(c.word, 0xFFFFFFFFu);

        expected = 0x7FFFFFFFu;
        EXPECT_FALSE(arch_cas_u32(&c.word, &expected, 0u));
        EXPECT_EQ(expected, 0xFFFFFFFFu);
        EXPECT_EQ(c.word, 0xFFFFFFFFu);

        expected = 0xFFFFFFFFu;
        EXPECT_TRUE(arch_cas_u32(&c.word, &expected, 0x80000001u));
        EXPECT_EQ(c.word, 0x80000001u);

        expected = 0x00000001u;
        EXPECT_FALSE(arch_cas_u32(&c.word, &expected, 0u));
        EXPECT_EQ(expected, 0x80000001u);
        EXPECT_EQ(c.word, 0x80000001u);
        EXPECT_EQ(c.lo, GUARD);
        EXPECT_EQ(c.hi, GUARD);
    }

#if defined(ATOMICCAS_BOUNDED)
    class ArchCasU32Bounded : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            g_attempts = 0;
            g_lose = 0;
            g_overwrites = false;
        }
    };

    // A store lost at every attempt ends the call with nothing stored and the word as read.
    TEST_F(ArchCasU32Bounded, a_store_lost_every_time_answers_false_after_the_bound)
    {
        Cell c{GUARD, 5u, GUARD};
        uint32_t expected = 5u;
        g_lose = LOSE_FOREVER;
        EXPECT_FALSE(arch_cas_u32(&c.word, &expected, 9u));
        EXPECT_EQ(g_attempts, ARCH_CAS_U32_STORES);
        EXPECT_EQ(expected, 5u);
        EXPECT_EQ(c.word, 5u);
        EXPECT_EQ(c.lo, GUARD);
        EXPECT_EQ(c.hi, GUARD);
    }

    TEST_F(ArchCasU32Bounded, a_store_landing_at_the_last_attempt_is_stored)
    {
        Cell c{GUARD, 5u, GUARD};
        uint32_t expected = 5u;
        g_lose = ARCH_CAS_U32_STORES - 1u;
        EXPECT_TRUE(arch_cas_u32(&c.word, &expected, 9u));
        EXPECT_EQ(g_attempts, ARCH_CAS_U32_STORES);
        EXPECT_EQ(c.word, 9u);
        EXPECT_EQ(expected, 5u);
    }

    // The next load sees what the store was lost to, and the call ends on it.
    TEST_F(ArchCasU32Bounded, a_store_lost_to_another_value_answers_that_value)
    {
        Cell c{GUARD, 5u, GUARD};
        uint32_t expected = 5u;
        g_lose = 1u;
        g_overwrites = true;
        g_overwrite = 0x80000006u;
        EXPECT_FALSE(arch_cas_u32(&c.word, &expected, 9u));
        EXPECT_EQ(g_attempts, 2u);
        EXPECT_EQ(expected, 0x80000006u);
        EXPECT_EQ(c.word, 0x80000006u);
    }

    TEST_F(ArchCasU32Bounded, a_miss_makes_one_attempt)
    {
        Cell c{GUARD, 7u, GUARD};
        uint32_t expected = 5u;
        EXPECT_FALSE(arch_cas_u32(&c.word, &expected, 9u));
        EXPECT_EQ(g_attempts, 1u);
        EXPECT_EQ(expected, 7u);
    }
#endif

#if defined(ATOMICCAS_MASKED)
    TEST(ArchCasU32Masked, LoadAndStoreSitInsideTheMask)
    {
        for (arch_irq_state_t const before : {arch_irq_state_t{0}, arch_irq_state_t{1}})
        {
            uint32_t word = 3u;
            uint32_t expected = 3u;
            g_masked = before;
            g_saves = 0u;
            g_restores = 0u;
            g_watch = &word;
            g_poke = 0u;
            g_at_restore = 0u;
            EXPECT_TRUE(arch_cas_u32(&word, &expected, 4u));
            EXPECT_EQ(g_at_restore, 4u);
            EXPECT_EQ(g_saves, 1u);
            EXPECT_EQ(g_restores, 1u);
            EXPECT_EQ(g_masked, before);

            g_poke = 8u;
            EXPECT_FALSE(arch_cas_u32(&word, &expected, 5u));
            EXPECT_EQ(expected, 8u);
            EXPECT_EQ(word, 8u);
            EXPECT_EQ(g_masked, before);
            g_watch = nullptr;
            g_poke = 0u;
        }
    }
#endif
}
