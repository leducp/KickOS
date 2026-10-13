// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The RP2350 claim: spinlock, owner and deadline. The arm is node 1, and plays node 2 by
// stealing the lock as a peer's try_take does.

#include "arch/arm/chip/rp2350/console_claim.h"

#include <gtest/gtest.h>

#include <stdint.h>

#include <string>

namespace
{
    namespace console = kickos::rp2350::console;

    constexpr uint32_t SELF = 1u;
    constexpr uint32_t PEER = 2u;
    constexpr uint32_t NODES = 2u;
    constexpr uint32_t STALL_US = 3u * console::HOLD_MAX_US;

    console::Word g_owner = 0;
    console::Word g_deadline = 0;
    console::Word g_waiting[NODES] = {};
    uint32_t g_now = 0;
    bool g_locked = false;
    // The order of the spinlock's accesses, the fences and the rings.
    std::string g_trace;

    // The peer's whole steal of an ended grant, taken a millisecond ago.
    void peer_steals(void)
    {
        g_locked = true; // freed under this node and taken again
        g_deadline = g_now + console::HOLD_MAX_US - 1000u;
        g_owner = PEER;
    }

    class Rp2350Claim : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            g_owner = 0;
            g_deadline = 0;
            g_now = 1000u;
            g_locked = false;
            g_waiting[0] = 0;
            g_waiting[1] = 0;
            g_trace.clear();
        }

        console::Claim claim{SELF, g_owner, g_deadline, g_waiting, NODES};

        void begin(void)
        {
            ASSERT_TRUE(claim.claim());
            ASSERT_TRUE(g_locked);
            ASSERT_EQ(uint32_t(g_owner), SELF);
        }
    };
}

namespace kickos::rp2350::console
{
    // Each read moves the clock on, so a wait for it ends.
    uint32_t now(void)
    {
        uint32_t const t = g_now;
        g_now++;
        return t;
    }

    bool lock_take(void)
    {
        g_trace += 'T';
        if (g_locked)
        {
            return false;
        }
        g_locked = true;
        return true;
    }

    void lock_free(void)
    {
        g_trace += 'U';
        g_locked = false;
    }

    void fence(void)
    {
        g_trace += 'F';
    }

    void ring(uint32_t node)
    {
        g_trace += "R" + std::to_string(node);
    }
}

TEST_F(Rp2350Claim, NeverRenewsAnEndedGrant)
{
    begin();
    g_now += STALL_US;
    EXPECT_FALSE(claim.claim());
    EXPECT_FALSE(claim.held());
    EXPECT_EQ(uint32_t(g_owner), 0u);
    EXPECT_FALSE(g_locked);
}

TEST_F(Rp2350Claim, NeverRenewsInsideTheMargin)
{
    begin();
    g_now = g_deadline - console::MARGIN_US / 2u;
    EXPECT_FALSE(claim.claim());
    EXPECT_FALSE(g_locked);
}

TEST_F(Rp2350Claim, LeavesAStolenGrantToThePeer)
{
    begin();
    g_now += STALL_US;
    peer_steals();
    uint32_t const peer_deadline = g_deadline;
    EXPECT_FALSE(claim.claim());
    EXPECT_FALSE(claim.held());
    EXPECT_EQ(uint32_t(g_owner), PEER);
    EXPECT_EQ(uint32_t(g_deadline), peer_deadline);
    EXPECT_TRUE(g_locked);
}

TEST_F(Rp2350Claim, AnUnfinishedLineKeepsItsLiveGrant)
{
    begin();
    claim.drop(false);
    EXPECT_TRUE(claim.held());
    EXPECT_TRUE(g_locked);
    g_now += STALL_US;
    claim.drop(false);
    EXPECT_FALSE(claim.held());
    EXPECT_FALSE(g_locked);
}

TEST_F(Rp2350Claim, AnOpenRetakesAnEndedGrant)
{
    begin();
    g_now += STALL_US;
    EXPECT_TRUE(claim.try_open());
    EXPECT_TRUE(claim.holding());
    EXPECT_EQ(uint32_t(g_owner), SELF);
}

TEST_F(Rp2350Claim, AnOpenWaitsOutAPeersLiveGrant)
{
    peer_steals();
    EXPECT_FALSE(claim.try_open());
    EXPECT_FALSE(claim.held());
    EXPECT_EQ(uint32_t(g_owner), PEER);
}

TEST_F(Rp2350Claim, StealsAGrantPastItsDeadline)
{
    peer_steals();
    g_now += STALL_US;
    EXPECT_TRUE(claim.try_open());
    EXPECT_EQ(uint32_t(g_owner), SELF);
    EXPECT_TRUE(g_locked);
}

// A waiter marks itself before it tries the spinlock, across a full fence, and a release frees
// the spinlock before it reads the marks, across another.
TEST_F(Rp2350Claim, AWaiterIsMarkedAheadOfItsOfferAndToldTheGrantsRest)
{
    peer_steals();
    uint32_t const rest = uint32_t(g_deadline) - g_now;
    g_trace.clear();
    uint32_t const park = claim.wait();
    EXPECT_EQ(uint32_t(g_waiting[SELF - 1u]), 1u);
    EXPECT_EQ(g_trace.substr(0, 2), "FT") << "the mark is not fenced ahead of the offer";
    EXPECT_GT(park, 0u);
    EXPECT_LE(park, rest);
}

// A waiter that takes the claim leaves no mark, so its own release rings no one.
TEST_F(Rp2350Claim, AWaiterTakesAFreeClaimAndClearsItsMark)
{
    EXPECT_EQ(claim.wait(), 0u);
    EXPECT_TRUE(claim.holding());
    EXPECT_EQ(uint32_t(g_waiting[SELF - 1u]), 0u)
        << "the waiter kept its mark after taking the claim";
}

// A spinlock held with no owner published yet has no deadline to wait out: a whole hold, which
// the release cuts short.
TEST_F(Rp2350Claim, AWaiterOnAnUnpublishedOwnerParksAWholeHold)
{
    g_locked = true;
    EXPECT_EQ(claim.wait(), console::HOLD_MAX_US);
}

TEST_F(Rp2350Claim, AReleaseRingsEveryMarkedNodeAndClearsItsMark)
{
    begin();
    g_waiting[PEER - 1u] = 1u;
    g_trace.clear();
    claim.drop(true);
    EXPECT_EQ(uint32_t(g_waiting[PEER - 1u]), 0u);
    EXPECT_EQ(g_trace, "FUFR1")
        << "the release does not fence its free ahead of reading the marks, or rings no one";
}

TEST_F(Rp2350Claim, AReleaseRingsNoUnmarkedNode)
{
    begin();
    g_trace.clear();
    claim.drop(true);
    EXPECT_EQ(g_trace, "FUF");
}

TEST_F(Rp2350Claim, AStolenGrantRingsNoOne)
{
    begin();
    g_now += STALL_US;
    peer_steals();
    g_waiting[PEER - 1u] = 1u;
    g_trace.clear();
    claim.drop(true);
    EXPECT_EQ(g_trace, "");
    EXPECT_EQ(uint32_t(g_waiting[PEER - 1u]), 1u);
}
