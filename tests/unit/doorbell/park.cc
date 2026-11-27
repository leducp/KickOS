// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The secondary park: every test for work runs masked, the sleep is reached only with no work and
// masked, another core's terminal halt stops the core before it can start, the terminal halt
// returns after its bound when a peer never stops, and a panic nested in the halter's own claim
// halts nobody. In a KICKOS_WITNESS_DOORBELL kernel, the spinning flag is cleared while the lock
// is still held.

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
        uint64_t clock = 0;
        uint64_t clock_step = 0;
        uint64_t spins = 0;
        uint64_t spin_limit = 0;
        uint32_t sent = 0;
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
#if defined(KICKOS_WITNESS_DOORBELL) && KICKOS_WITNESS_DOORBELL
            g_contend = CONTEND_PARK;
            g_spinning[1].v = 0u;
            g_held[1].v = 0u;
#endif
            g_halter = 0u;
            g_halted[0] = 0u;
            g_halted[1] = 0u;
            g_reachable[0] = 0u;
            g_reachable[1] = 0u;
            g_halt_claim = 0u;
        }

        // True when the halt returned; false when the spin limit cut it off.
        bool halt(void)
        {
            fix::g_on_spin = count_spin;
            if (setjmp(g_exit) == 0)
            {
                halt_peers();
                return true;
            }
            return false;
        }

        static void count_spin(void)
        {
            fix::g_log.clear();
            g.clock += g.clock_step;
            g.spins++;
            if (g.spins >= g.spin_limit)
            {
                longjmp(g_exit, 1);
            }
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

#if defined(KICKOS_WITNESS_DOORBELL) && KICKOS_WITNESS_DOORBELL
namespace kickos::doorbell
{
    Seq g_contend = {};
    PartCell g_held[KICKOS_NUM_CORES] = {};
    PartCell g_spinning[KICKOS_NUM_CORES] = {};
}
#endif

namespace kickos::doorbell
{
    Seq g_halter = {};
    Seq g_halted[KICKOS_DOORBELL_CORES] = {};
    Seq g_reachable[KICKOS_DOORBELL_CORES] = {};
    uint32_t g_halt_claim = 0u;
}

namespace
{
    void leave_halted(void)
    {
        longjmp(g_exit, 1);
    }
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

    uint64_t arch_clock_now(void)
    {
        return g.clock;
    }

    void arch_ipi_send(uint32_t cores)
    {
        g.sent |= cores;
    }

    void arch_kernel_unlock(void)
    {
#if defined(KICKOS_WITNESS_DOORBELL) && KICKOS_WITNESS_DOORBELL
        g.spinning_at_unlock += g_spinning[1].v.load();
        if (g.lock_rounds >= g.lock_rounds_before_park)
        {
            g_contend = CONTEND_PARK;
        }
#endif
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

TEST_F(DoorbellPark, AnotherCoresHaltStopsTheCoreBeforeItStarts)
{
    g.startable_after = 0;
    g_halter = 1u;
    fix::g_on_spin = leave_halted;
    run();
    EXPECT_EQ(g.started, 0u) << "a core asked to halt started its scheduler";
    EXPECT_EQ(g_halted[1].load(), 1u);
    EXPECT_EQ(g_reachable[1].load(), 1u);
    EXPECT_TRUE(fix::g_masked) << "the halted core spins unmasked";
}

TEST_F(DoorbellPark, ItsOwnHaltDoesNotStopTheCore)
{
    g.startable_after = 0;
    g_halter = 2u;
    run();
    EXPECT_EQ(g.started, 1u);
    EXPECT_EQ(g_halted[1].load(), 0u);
}

#if defined(KICKOS_WITNESS_DOORBELL) && KICKOS_WITNESS_DOORBELL
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
#endif

TEST_F(DoorbellPark, TheHaltReturnsAfterItsBoundWhenAPeerNeverStops)
{
    g_reachable[0] = 1u;
    g.clock = 1000u;
    g.clock_step = 1000u * 1000u;
    g.spin_limit = 1000000u;
    EXPECT_TRUE(halt()) << "the halt waited past its bound on a peer that never stops";
    EXPECT_EQ(g.sent, 1u);
    EXPECT_EQ(g_halter.load(), 2u);
    EXPECT_EQ(g_halted[1].load(), 0u);
    EXPECT_GE(g.clock - 1000u, PEER_WAIT_NS);
    EXPECT_LE(g.clock - 1000u, PEER_WAIT_NS + 2u * g.clock_step);
}

TEST_F(DoorbellPark, TheHaltReturnsUnderADeadClockWhenAPeerNeverStops)
{
    g_reachable[0] = 1u;
    g.clock = 1000u;
    g.spin_limit = 4u * DEAD_CLOCK_SPINS;
    EXPECT_TRUE(halt()) << "the halt waited forever on a dead clock";
    EXPECT_EQ(g_halted[1].load(), 0u);
}

TEST_F(DoorbellPark, TheHaltWaitsForAPeerThatStops)
{
    g_reachable[0] = 1u;
    g_halted[0] = 1u;
    g.spin_limit = 1u;
    EXPECT_TRUE(halt());
    EXPECT_EQ(g.spins, 0u);
}

TEST_F(DoorbellPark, APanicNestedInTheHaltersClaimHaltsNobody)
{
    // The state an outer halt on this core leaves between its claim and its g_halter store.
    g_halt_claim = 2u;
    g.spin_limit = 1000u;
    EXPECT_TRUE(halt()) << "the claiming core halted itself, so nobody blinks";
    EXPECT_EQ(g_halted[1].load(), 0u);
    EXPECT_EQ(g_halter.load(), 2u);
    EXPECT_EQ(g.sent, 1u) << "the nested halt did not ask the peers the outer one never asked";
}

TEST_F(DoorbellPark, AHaltClaimedByAnotherCoreHaltsThisOne)
{
    g_halt_claim = 1u;
    g_halter = 1u;
    g.spin_limit = 10u;
    EXPECT_FALSE(halt());
    EXPECT_EQ(g_halted[1].load(), 1u);
    EXPECT_EQ(g.sent, 0u);
}
