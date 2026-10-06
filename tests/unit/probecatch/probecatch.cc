// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The probe read swallows its own load's fault, armed, from a privileged context, and no other.

#include "arch/arm/armv7m/probe_catch.h"

#include <gtest/gtest.h>

#include <stdint.h>

namespace
{
    constexpr uint32_t LOAD = 0x10001000u;
    constexpr uint32_t TO_HANDLER = 0xFFFFFFF1u;
    constexpr uint32_t TO_THREAD_PSP = 0xFFFFFFFDu;
    constexpr uint32_t PRIVILEGED = 0x0u;
    constexpr uint32_t UNPRIVILEGED = 0x1u;
    constexpr uint32_t PRECISERR_BFARVALID = (1u << 9) | (1u << 15);
    constexpr uintptr_t AT = 0x50000000u;

    bool claims(uint32_t pc, bool armed, uint32_t exc_return, uint32_t control)
    {
        return kickos::armv7m::probe_claims(pc, LOAD, armed, exc_return, control);
    }

    kickos::armv7m::ProbeLatch g_latch = {};
    uint32_t g_frame[8] = {};
    bool g_caught = false;

    // The probe's load faulting: the fault path runs over the frame the load stacked.
    uint32_t faulting_load(uintptr_t at)
    {
        g_frame[6] = LOAD;
        g_caught = kickos::armv7m::probe_catch(g_latch, g_frame, LOAD, TO_THREAD_PSP, PRIVILEGED,
                                               PRECISERR_BFARVALID, static_cast<uint32_t>(at));
        return 0xDEADBEEFu;
    }

    uint32_t clean_load(uintptr_t)
    {
        return 0x12345678u;
    }
}

TEST(ProbeCatch, ArmedLoadFromHandlerModeIsClaimed)
{
    EXPECT_TRUE(claims(LOAD, true, TO_HANDLER, UNPRIVILEGED));
}

TEST(ProbeCatch, ArmedLoadFromPrivilegedThreadIsClaimed)
{
    EXPECT_TRUE(claims(LOAD | 1u, true, TO_THREAD_PSP, PRIVILEGED));
}

TEST(ProbeCatch, UnarmedLoadIsNotClaimed)
{
    EXPECT_FALSE(claims(LOAD, false, TO_HANDLER, PRIVILEGED));
}

TEST(ProbeCatch, UnprivilegedThreadIsNotClaimedWhileArmed)
{
    EXPECT_FALSE(claims(LOAD, true, TO_THREAD_PSP, UNPRIVILEGED));
}

TEST(ProbeCatch, AnotherInstructionIsNotClaimed)
{
    EXPECT_FALSE(claims(LOAD + 2u, true, TO_HANDLER, PRIVILEGED));
}

TEST(ProbeRead, AFaultInsideTheReadIsCaughtAndReported)
{
    g_latch = {};
    g_caught = false;
    uint32_t value = 0u;
    EXPECT_EQ(kickos::armv7m::probe_read(g_latch, faulting_load, AT, &value), PRECISERR_BFARVALID);
    EXPECT_TRUE(g_caught);
    EXPECT_EQ(value, static_cast<uint32_t>(AT));
    EXPECT_EQ(g_frame[6], LOAD + 2u);
}

TEST(ProbeRead, AFaultAfterTheReadIsNotCaught)
{
    g_latch = {};
    uint32_t value = 0u;
    EXPECT_EQ(kickos::armv7m::probe_read(g_latch, clean_load, AT, &value), 0u);
    EXPECT_EQ(value, 0x12345678u);
    g_frame[6] = LOAD;
    EXPECT_FALSE(kickos::armv7m::probe_catch(g_latch, g_frame, LOAD, TO_THREAD_PSP, PRIVILEGED,
                                             PRECISERR_BFARVALID, static_cast<uint32_t>(AT)));
    EXPECT_EQ(g_frame[6], LOAD);
}
