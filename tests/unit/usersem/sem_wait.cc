// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// kos_sem_wait over the real user/src/sem.cc and the sim's compare-and-swap. The kernel here
// answers the word wait as a script says, each wait running for a set time on a clock only the
// script advances.

#include <gtest/gtest.h>

#include <string>

#include <kickos/arch/arch.h>
#include <kickos/sys.h>

namespace
{
    constexpr uint32_t TIMEOUT_US = 1000;
    constexpr uint32_t RUNS_US = 400;
    // An arm whose waits never run out stops here.
    constexpr uint32_t WAITS_MAX = 16;

    uint64_t g_now_ns = 0;
    uint32_t g_clock_reads = 0;
    uint32_t g_waits = 0;
    uint32_t g_handed_us[WAITS_MAX] = {};
    // What a wait answers once it has run RUNS_US short of its timeout, and the wait, counted
    // from 1, that is handed a unit instead (0: none is).
    int g_answer = 0;
    uint32_t g_unit_at = 0;

    kos_sem_t g_sem = KOS_SEM_INIT(0);

    class SemWait : public ::testing::TestWithParam<int>
    {
      protected:
        void SetUp() override
        {
            g_now_ns = 0;
            g_clock_reads = 0;
            g_waits = 0;
            g_answer = GetParam();
            g_unit_at = 0;
            g_sem.word = 0;
        }
    };
}

extern "C" uint64_t kos_clock_now(void)
{
    g_clock_reads++;
    return g_now_ns;
}

extern "C" uintptr_t arch_syscall(uintptr_t nr, uintptr_t, uintptr_t, uintptr_t a2, uintptr_t)
{
    if (nr != KOS_SYS_WORD_WAIT)
    {
        ADD_FAILURE() << "the semaphore made syscall " << nr;
        return static_cast<uintptr_t>(-KOS_ENOSYS);
    }
    uint32_t const timeout_us = static_cast<uint32_t>(a2);
    if (g_waits < WAITS_MAX)
    {
        g_handed_us[g_waits] = timeout_us;
    }
    g_waits++;
    if (g_waits == g_unit_at)
    {
        g_now_ns += static_cast<uint64_t>(RUNS_US) * 1000u;
        return 1u;
    }
    if (g_waits >= WAITS_MAX or (timeout_us != KOS_TIMEOUT_NONE and timeout_us <= RUNS_US))
    {
        g_now_ns += static_cast<uint64_t>(timeout_us) * 1000u;
        return static_cast<uintptr_t>(-KOS_ETIMEDOUT);
    }
    g_now_ns += static_cast<uint64_t>(RUNS_US) * 1000u;
    return static_cast<uintptr_t>(g_answer);
}

// A wake with no unit, or a word changed under the wait, re-waits only for what is left of the
// timeout, so the wait as a whole ends within it.
TEST_P(SemWait, a_wait_woken_without_a_unit_ends_within_its_timeout)
{
    EXPECT_EQ(kos_sem_wait(&g_sem, TIMEOUT_US), -KOS_ETIMEDOUT);

    EXPECT_LE(g_now_ns, static_cast<uint64_t>(TIMEOUT_US) * 1000u)
        << "the wait ran past its timeout over " << g_waits << " waits";
    ASSERT_GE(g_waits, 2u);
    EXPECT_EQ(g_handed_us[0], TIMEOUT_US);
    EXPECT_EQ(g_handed_us[1], TIMEOUT_US - RUNS_US) << "a re-wait was handed the whole timeout";
}

TEST_P(SemWait, an_untimed_wait_reads_no_clock_and_rewaits_forever)
{
    g_unit_at = 3;

    EXPECT_EQ(kos_sem_wait(&g_sem, KOS_TIMEOUT_NONE), 0);

    EXPECT_EQ(g_waits, 3u);
    EXPECT_EQ(g_handed_us[1], KOS_TIMEOUT_NONE);
    EXPECT_EQ(g_handed_us[2], KOS_TIMEOUT_NONE);
    EXPECT_EQ(g_clock_reads, 0u) << "an untimed wait read the clock";
}

TEST_P(SemWait, a_take_or_a_try_reads_no_clock_and_makes_no_wait)
{
    g_sem.word = 1u;
    EXPECT_EQ(kos_sem_wait(&g_sem, TIMEOUT_US), 0);
    EXPECT_EQ(kos_sem_wait(&g_sem, 0u), -KOS_ETIMEDOUT);

    EXPECT_EQ(g_sem.word, 0u);
    EXPECT_EQ(g_waits, 0u);
    EXPECT_EQ(g_clock_reads, 0u) << "a wait that never parked read the clock";
}

INSTANTIATE_TEST_SUITE_P(Rewait, SemWait, ::testing::Values(0, -KOS_EAGAIN),
                         [](::testing::TestParamInfo<int> const& p)
                         {
                             if (p.param == 0)
                             {
                                 return std::string{"woken_without_a_unit"};
                             }
                             return std::string{"word_changed"};
                         });
