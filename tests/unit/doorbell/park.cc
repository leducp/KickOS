// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The secondary park: every test for work runs masked, the sleep is reached only with no work and
// masked, and the spinning flag is cleared while the lock is still held.

#include "seam.h"

#include <kickos/arch/doorbell_protocol.h>

#include <gtest/gtest.h>

#include <setjmp.h>

namespace
{
    namespace fix = kickos::doorbellfix;
    using namespace kickos::doorbell;

    struct State
    {
        uint32_t startable_after = 0;
        uint32_t tests = 0;
        uint32_t tested_open = 0;
        uint32_t sleeps = 0;
        uint32_t slept_open = 0;
        uint32_t slept_with_work = 0;
        uint32_t lock_rounds = 0;
        uint32_t lock_rounds_before_park = 0;
        uint32_t spinning_at_unlock = 0;
        uint32_t started = 0;
        bool work = false;
    };
    State g;
    jmp_buf g_exit;

    struct HostPart
    {
        static void sleep(void)
        {
            g.sleeps++;
            if (not fix::g_masked)
            {
                g.slept_open++;
            }
            if (g.work)
            {
                g.slept_with_work++;
            }
            fix::g_masked = false;
        }
        static void open(void)
        {
            fix::g_masked = false;
        }
        static void before_start(void)
        {
        }
    };

    class DoorbellPark : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            fix::reset();
            g = State();
            g_contend = CONTEND_PARK;
            g_spinning[1].v = 0u;
            g_held[1].v = 0u;
        }

        void run(void)
        {
            if (setjmp(g_exit) == 0)
            {
                park<HostPart>();
            }
        }
    };
}

namespace kickos::doorbell
{
    Seq g_contend = {};
    PartCell g_held[KICKOS_NUM_CORES] = {};
    PartCell g_spinning[KICKOS_NUM_CORES] = {};
}

extern "C"
{
    int kickos_kernel_core_startable(void)
    {
        g.tests++;
        if (not fix::g_masked)
        {
            g.tested_open++;
        }
        if (g.tests > g.startable_after)
        {
            g.work = true;
        }
        return static_cast<int>(g.work);
    }

    void kickos_kernel_core_start(void)
    {
        g.started++;
        longjmp(g_exit, 1);
    }

    void arch_kernel_lock(void)
    {
        g.lock_rounds++;
    }

    void arch_kernel_unlock(void)
    {
        g.spinning_at_unlock += g_spinning[1].v.load();
        if (g.lock_rounds >= g.lock_rounds_before_park)
        {
            g_contend = CONTEND_PARK;
        }
    }
}

TEST_F(DoorbellPark, WorkPresentStartsWithoutSleeping)
{
    g.startable_after = 0;
    run();
    EXPECT_EQ(g.started, 1u);
    EXPECT_EQ(g.sleeps, 0u) << "a core with work slept";
    EXPECT_EQ(g.tested_open, 0u);
}

TEST_F(DoorbellPark, SleepsOnlyMaskedAndWithNoWork)
{
    g.startable_after = 3;
    run();
    EXPECT_EQ(g.started, 1u);
    EXPECT_EQ(g.sleeps, 3u);
    EXPECT_EQ(g.tested_open, 0u) << "a test for work ran with interrupts open";
    EXPECT_EQ(g.slept_open, 0u) << "the sleep was entered unmasked, so a raise could land first";
    EXPECT_EQ(g.slept_with_work, 0u);
}

TEST_F(DoorbellPark, SpinningIsClearedUnderTheLock)
{
    g_contend = CONTEND_LOCK;
    g.lock_rounds_before_park = 2;
    g.startable_after = 4;
    run();
    EXPECT_EQ(g.lock_rounds, 2u);
    EXPECT_EQ(g_held[1].v.load(), 2u);
    EXPECT_EQ(g.spinning_at_unlock, 0u) << "the spinning flag outlived the lock";
    EXPECT_EQ(g_spinning[1].v.load(), 0u);
    EXPECT_EQ(g.tested_open, 0u);
}
