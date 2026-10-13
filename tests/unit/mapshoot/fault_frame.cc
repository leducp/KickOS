// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Which frames on a thread's own kernel block the x86_64 backend attributes to the thread.

#include <kickos/arch/trap.h>

#include <gtest/gtest.h>

using kickos::x86_64::trap_frame;
using kickos::x86_64::trap_frame_from_ring3;

namespace
{
    constexpr uint64_t CS_KERNEL = 0x08;
    constexpr uint64_t CS_USER = 0x2b;
    constexpr uint64_t VECTOR_PAGE_FAULT = 14;
    constexpr uint64_t VECTOR_TIMER = 32;

    trap_frame frame(uint64_t vector, uint64_t cs)
    {
        trap_frame f = {};
        f.vector = vector;
        f.cs = cs;
        return f;
    }
}

TEST(FaultFrame, AnExceptionTheThreadTookAtRing3IsItsOwn)
{
    EXPECT_TRUE(trap_frame_from_ring3(frame(VECTOR_PAGE_FAULT, CS_USER)));
}

TEST(FaultFrame, AKernelFaultDuringADispatchOnTheSameBlockIsNot)
{
    EXPECT_FALSE(trap_frame_from_ring3(frame(VECTOR_PAGE_FAULT, CS_KERNEL)));
}

TEST(FaultFrame, AnExternalDeliveryIsNotWhateverItsPushedSelectorSays)
{
    EXPECT_FALSE(trap_frame_from_ring3(frame(VECTOR_TIMER, CS_USER)));
}
