// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The poll and the lock's turn wait. After the clear that absorbed a raise, whatever the cells
// still say this core is owed is raised again on it, or the reschedule or the device line it
// carried is lost.

#include "seam.h"

#include <kickos/arch/doorbell_protocol.h>

#include <gtest/gtest.h>

namespace
{
    namespace fix = kickos::doorbellfix;

    struct State
    {
        bool pending = false;
        bool inject_owed = false;
        bool resched_owed = false;
        uint32_t clears = 0;
        uint32_t services = 0;
        uint32_t raises = 0;
        bool raised_masked = false;
    };
    State g;

    struct HostPart
    {
        static void clear(void)
        {
            g.clears++;
            fix::g_log.push_back('C');
        }
        static void raise_self(void)
        {
            g.raises++;
            g.raised_masked = fix::g_masked;
            fix::g_log.push_back('R');
        }
        static bool inject_owed(void)
        {
            return g.inject_owed;
        }
    };

    class DoorbellPoll : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            fix::reset();
            g = State();
        }
    };
}

namespace kickos::doorbell
{
    bool pending(void)
    {
        return g.pending;
    }
}

extern "C"
{
    void kickos_doorbell_service(void)
    {
        g.services++;
        fix::g_log.push_back('V');
    }

    int kickos_kernel_core_resched_owed(void)
    {
        return static_cast<int>(g.resched_owed);
    }

    void kickos_doorbell_poll(void)
    {
        fix::g_log.push_back('P');
    }
}

TEST_F(DoorbellPoll, NothingPendingLeavesThePendingStateAlone)
{
    g.resched_owed = true;
    kickos::doorbell::poll<HostPart>();
    EXPECT_EQ(g.clears, 0u) << "a raise the poll cannot see must stand until the unmask";
    EXPECT_EQ(g.services, 0u);
    EXPECT_EQ(g.raises, 0u);
}

TEST_F(DoorbellPoll, ClearsBeforeTheService)
{
    g.pending = true;
    kickos::doorbell::poll<HostPart>();
    EXPECT_EQ(fix::g_log, "CV") << "a raise landing between the service and the clear is erased";
}

TEST_F(DoorbellPoll, AnOwedRescheduleIsRaisedAgain)
{
    g.pending = true;
    g.resched_owed = true;
    kickos::doorbell::poll<HostPart>();
    EXPECT_EQ(g.raises, 1u) << "the clear dropped the reschedule's raise and nothing restored it";
    EXPECT_EQ(fix::g_log, "CVR");
    EXPECT_TRUE(g.raised_masked);
    EXPECT_FALSE(fix::g_masked);
}

TEST_F(DoorbellPoll, AnOwedInjectionIsRaisedAgain)
{
    g.pending = true;
    g.inject_owed = true;
    kickos::doorbell::poll<HostPart>();
    EXPECT_EQ(g.raises, 1u) << "the clear dropped the device line's raise and nothing restored it";
    EXPECT_EQ(fix::g_log, "CVR");
}

TEST_F(DoorbellPoll, BothOwedRaiseOnce)
{
    g.pending = true;
    g.inject_owed = true;
    g.resched_owed = true;
    kickos::doorbell::poll<HostPart>();
    EXPECT_EQ(g.raises, 1u);
}

TEST_F(DoorbellPoll, NothingOwedRaisesNothing)
{
    g.pending = true;
    kickos::doorbell::poll<HostPart>();
    EXPECT_EQ(g.services, 1u);
    EXPECT_EQ(g.raises, 0u);
}

TEST(DoorbellTurn, TestsTheTurnBeforeEachPoll)
{
    fix::reset();
    uint32_t misses = 2;
    kickos::doorbell::await_turn([&misses] {
        fix::g_log.push_back('T');
        if (misses == 0)
        {
            return true;
        }
        misses--;
        return false;
    });
    EXPECT_EQ(fix::g_log, "TPSTPST") << "a poll ran ahead of the turn test";
}

TEST(DoorbellTurn, ATurnAlreadyComeNeitherPollsNorSpins)
{
    fix::reset();
    kickos::doorbell::await_turn([] {
        fix::g_log.push_back('T');
        return true;
    });
    EXPECT_EQ(fix::g_log, "T");
}
