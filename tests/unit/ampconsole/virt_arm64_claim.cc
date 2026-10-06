// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The virt_arm64 claim word, through the writer each console entry point uses. The arm is node
// 1, and plays node 2 by writing the word itself.

#include "arch/arm64/chip/virt_arm64/console_claim.h"

#include <gtest/gtest.h>

#include <stdint.h>

#include <set>
#include <string>

namespace
{
    namespace console = kickos::virt_arm64::console;

    constexpr uint32_t BUDGET = 6400u; // a margin of 100 ticks
    constexpr uint32_t STALL = 3u * BUDGET;
    constexpr uint32_t SELF = 1u;
    constexpr uint32_t PEER = 2u;

    uint64_t g_word = 0;
    uint32_t g_now = 0;
    uint32_t g_spurious = 0; // compare-exchanges to fail with the word unchanged
    std::string g_wire;

    // Before the byte at each of these wire offsets, the clock jumps by g_stall.
    std::set<size_t> g_stall_at;
    uint32_t g_stall = STALL;
    // At the first stall, the peer takes the ended grant.
    bool g_peer_steals = false;
    // The peer read the word at the first stall, and will offer it to its own swap later.
    uint64_t g_peer_seen = 0;
    // At the first stall: the peer's swap lands just ahead of the next one this node makes,
    // and that many of this node's swaps then fail with the word unchanged.
    bool g_peer_swaps_first = false;
    bool g_peer_swaps_after_stall = false;
    uint32_t g_spurious_after_stall = 0;

    uint64_t token(uint32_t deadline, uint32_t owner)
    {
        return (uint64_t(deadline) << 32) | owner;
    }

    std::string bytes(size_t n, char c)
    {
        return std::string(n, c);
    }

    class VirtArm64Claim : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            g_word = 0;
            g_now = 1000u;
            g_spurious = 0;
            g_wire.clear();
            g_stall_at.clear();
            g_stall = STALL;
            g_peer_steals = false;
            g_peer_seen = 0;
            g_peer_swaps_first = false;
            g_peer_swaps_after_stall = false;
            g_spurious_after_stall = 0;
        }

        console::Claim claim{SELF};

        size_t kernel(std::string const& s) { return claim.kernel_write(s.data(), s.size(), BUDGET); }

        size_t user(std::string const& s)
        {
            bool cr_pending = false;
            return claim.user_write(s.data(), s.size(), &cr_pending, BUDGET);
        }
    };
}

namespace kickos::virt_arm64::console
{
    uint32_t ticks(void)
    {
        return g_now;
    }

    uint64_t load(void)
    {
        return g_word;
    }

    bool compare_exchange(uint64_t& expected, uint64_t desired)
    {
        if (g_peer_swaps_first)
        {
            g_peer_swaps_first = false;
            g_word = token(g_now + BUDGET, PEER);
        }
        if (g_word != expected)
        {
            expected = g_word;
            return false;
        }
        if (g_spurious != 0)
        {
            g_spurious--;
            return false;
        }
        g_word = desired;
        return true;
    }

    bool wait_room(void)
    {
        if (g_stall_at.erase(g_wire.size()) != 0)
        {
            g_now += g_stall;
            g_peer_seen = g_word;
            g_peer_swaps_first = g_peer_swaps_after_stall;
            g_spurious = g_spurious_after_stall;
            if (g_peer_steals)
            {
                g_peer_steals = false;
                g_word = token(g_now + BUDGET, PEER);
            }
        }
        return true;
    }

    void store(char c)
    {
        g_wire.push_back(c);
    }
}

TEST_F(VirtArm64Claim, KernelLineRenewsAnEndedGrantNoPeerTook)
{
    g_stall_at = {1u};
    EXPECT_EQ(kernel("ab\n"), 3u);
    EXPECT_EQ(g_wire, "ab\n");
    EXPECT_EQ(g_word, 0u);
}

TEST_F(VirtArm64Claim, KernelLineRenewsInsideTheMargin)
{
    g_stall_at = {1u};
    g_stall = BUDGET - BUDGET / 128u;
    EXPECT_EQ(kernel("ab"), 2u);
    EXPECT_EQ(g_word, token(g_now + BUDGET, SELF));
}

TEST_F(VirtArm64Claim, PolledLineRenewsAnEndedGrantNoPeerTook)
{
    g_stall_at = {1u};
    claim.polled_write("ab\n", 3u, BUDGET);
    EXPECT_EQ(g_wire, "ab\n");
}

// A line lost to a peer mid-line is dropped up to its newline, across the calls carrying it: its
// tail never resumes after the peer's output. The next line goes out whole.
TEST_F(VirtArm64Claim, APolledLineLostMidLineStaysDroppedAcrossCalls)
{
    g_stall_at = {1u};
    g_peer_steals = true;
    EXPECT_TRUE(claim.polled_write("ab", 2u, BUDGET));
    g_word = 0; // the peer's line is done
    EXPECT_TRUE(claim.polled_write("\r\n", 2u, BUDGET));
    EXPECT_EQ(g_wire, "a");
    EXPECT_TRUE(claim.polled_write("cd\n", 3u, BUDGET));
    EXPECT_EQ(g_wire, "acd\n");
}

TEST_F(VirtArm64Claim, UserWriteStopsAtAnEndedGrant)
{
    g_stall_at = {1u};
    EXPECT_EQ(user("ab\n"), 1u);
    EXPECT_EQ(g_wire, "a");
    EXPECT_EQ(g_word, 0u);
    EXPECT_FALSE(claim.held());
}

TEST_F(VirtArm64Claim, UserWriteStopsInsideTheMargin)
{
    g_stall_at = {1u};
    g_stall = BUDGET - BUDGET / 128u;
    EXPECT_EQ(user("ab"), 1u);
    EXPECT_EQ(g_word, 0u);
}

TEST_F(VirtArm64Claim, NoRenewalOnceAPeerTookTheGrant)
{
    g_stall_at = {1u};
    g_peer_steals = true;
    EXPECT_EQ(kernel("ab\n"), 1u);
    EXPECT_EQ(g_word, token(g_now + BUDGET, PEER));
    EXPECT_EQ(g_wire, "a");
}

TEST_F(VirtArm64Claim, APeerSwapLandingAheadOfTheRenewalWins)
{
    g_stall_at = {1u};
    g_peer_swaps_after_stall = true;
    EXPECT_EQ(kernel("ab\n"), 1u);
    EXPECT_EQ(g_word, token(g_now + BUDGET, PEER));
    EXPECT_EQ(g_wire, "a");
}

TEST_F(VirtArm64Claim, APeerStealDecidedOnTheOldWordLosesToTheRenewal)
{
    g_stall_at = {1u};
    EXPECT_EQ(kernel("ab"), 2u);
    uint64_t const ours = g_word;
    ASSERT_EQ(ours, token(g_now + BUDGET, SELF));
    ASSERT_NE(g_peer_seen, ours);
    uint64_t expected = g_peer_seen;
    EXPECT_FALSE(console::compare_exchange(expected, token(g_now + BUDGET, PEER)));
    EXPECT_EQ(g_word, ours);
    EXPECT_TRUE(claim.holding(BUDGET));
}

TEST_F(VirtArm64Claim, RenewalOutlastsAFailedStoreExclusive)
{
    g_stall_at = {1u};
    g_spurious_after_stall = 3u;
    EXPECT_EQ(kernel("ab"), 2u);
    EXPECT_EQ(g_word, token(g_now + BUDGET, SELF));
}

TEST_F(VirtArm64Claim, RenewalEndsAtTheKernelLineBound)
{
    g_stall_at = {KICKOS_DIAG_LINE_MAX - 1u, KICKOS_DIAG_LINE_MAX};
    EXPECT_EQ(kernel(bytes(KICKOS_DIAG_LINE_MAX + 1u, 'x')), size_t(KICKOS_DIAG_LINE_MAX));
    EXPECT_EQ(g_word, 0u);
}

TEST_F(VirtArm64Claim, AFreshGrantRestartsTheBound)
{
    ASSERT_EQ(kernel(bytes(KICKOS_DIAG_LINE_MAX, 'x') + "\n"), KICKOS_DIAG_LINE_MAX + 1u);
    g_stall_at = {g_wire.size() + 1u};
    EXPECT_EQ(kernel("ab"), 2u);
}

TEST_F(VirtArm64Claim, UserBytesDoNotSpendAKernelLinesBound)
{
    ASSERT_EQ(user(bytes(KICKOS_DIAG_LINE_MAX, 'u')), size_t(KICKOS_DIAG_LINE_MAX));
    ASSERT_TRUE(claim.held());
    g_stall_at = {g_wire.size() + 1u};
    EXPECT_EQ(kernel("ab"), 2u);
}
