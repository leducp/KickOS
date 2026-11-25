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
    constexpr int64_t DEADLINE_NS = 1000000;
    constexpr int64_t RUNS_NS = 400000;
    // An arm whose waits never run out stops here.
    constexpr uint32_t WAITS_MAX = 16;

    int64_t g_now_ns = 0;
    uint32_t g_clock_reads = 0;
    uint32_t g_waits = 0;
    uint64_t g_handed[WAITS_MAX] = {};
    uintptr_t g_kinds[WAITS_MAX] = {};
    // What a wait answers once it has run RUNS_NS short of its deadline, and the wait, counted
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

extern "C" int64_t kickos_monotonic_now(void)
{
    g_clock_reads++;
    return g_now_ns;
}

extern "C" uintptr_t arch_syscall(uintptr_t nr, uintptr_t, uintptr_t, uintptr_t a2, uintptr_t a3,
                                  uintptr_t a4)
{
    if (nr != KOS_SYS_WORD_WAIT)
    {
        ADD_FAILURE() << "the semaphore made syscall " << nr;
        return static_cast<uintptr_t>(-KOS_ENOSYS);
    }
    uint64_t const limit = kos_u64_join(static_cast<uint32_t>(a2), static_cast<uint32_t>(a3));
    if (g_waits < WAITS_MAX)
    {
        g_handed[g_waits] = limit;
        g_kinds[g_waits] = a4;
    }
    g_waits++;
    if (g_waits == g_unit_at)
    {
        g_now_ns += RUNS_NS;
        return 1u;
    }
    if (g_waits >= WAITS_MAX
        or (limit != KOS_LIMIT_NONE and static_cast<int64_t>(limit) - g_now_ns <= RUNS_NS))
    {
        if (limit != KOS_LIMIT_NONE and static_cast<int64_t>(limit) > g_now_ns)
        {
            g_now_ns = static_cast<int64_t>(limit);
        }
        return static_cast<uintptr_t>(-KOS_ETIMEDOUT);
    }
    g_now_ns += RUNS_NS;
    return static_cast<uintptr_t>(g_answer);
}

// A wake with no unit, or a word changed under the wait, re-waits toward the same deadline, so
// the wait as a whole ends at it with no clock read but the one ahead of setting the waiters bit.
TEST_P(SemWait, a_wait_woken_without_a_unit_ends_at_its_deadline)
{
    EXPECT_EQ(kos_sem_wait(&g_sem, kos_monotonic_t{DEADLINE_NS}), -KOS_ETIMEDOUT);

    EXPECT_LE(g_now_ns, DEADLINE_NS) << "the wait ran past its deadline over " << g_waits
                                     << " waits";
    ASSERT_GE(g_waits, 2u);
    EXPECT_EQ(g_handed[0], static_cast<uint64_t>(DEADLINE_NS));
    EXPECT_EQ(g_handed[1], static_cast<uint64_t>(DEADLINE_NS)) << "a re-wait moved the deadline";
    EXPECT_EQ(g_kinds[0], static_cast<uintptr_t>(KOS_LIMIT_MONOTONIC));
    EXPECT_EQ(g_kinds[1], static_cast<uintptr_t>(KOS_LIMIT_MONOTONIC));
    EXPECT_EQ(g_clock_reads, 1u) << "a re-wait read the clock again";
}

TEST_P(SemWait, a_passed_deadline_is_a_try_that_leaves_no_waiters_bit)
{
    g_now_ns = DEADLINE_NS;

    EXPECT_EQ(kos_sem_wait(&g_sem, kos_monotonic_t{DEADLINE_NS}), -KOS_ETIMEDOUT);
    EXPECT_EQ(kos_sem_wait(&g_sem, kos_monotonic_t{1}), -KOS_ETIMEDOUT);

    EXPECT_EQ(g_sem.word, 0u) << "a passed deadline set the waiters bit";
    EXPECT_EQ(g_waits, 0u) << "a passed deadline entered the kernel";
}

TEST_P(SemWait, an_untimed_wait_reads_no_clock_and_rewaits_forever)
{
    g_unit_at = 3;

    EXPECT_EQ(kos_sem_wait(&g_sem, KOS_NEVER), 0);

    EXPECT_EQ(g_waits, 3u);
    EXPECT_EQ(g_handed[1], KOS_LIMIT_NONE);
    EXPECT_EQ(g_handed[2], KOS_LIMIT_NONE);
    EXPECT_EQ(g_clock_reads, 0u) << "an untimed wait read the clock";
}

TEST_P(SemWait, a_take_or_a_try_reads_no_clock_and_makes_no_wait)
{
    g_sem.word = 1u;
    EXPECT_EQ(kos_sem_wait(&g_sem, kos_monotonic_t{DEADLINE_NS}), 0);
    EXPECT_EQ(kos_sem_wait(&g_sem, KOS_POLL), -KOS_EAGAIN);

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
