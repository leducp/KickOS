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
    constexpr uint64_t VECTOR_DIVIDE = 0;
    constexpr uint64_t VECTOR_NMI = 2;
    constexpr uint64_t VECTOR_DOUBLE_FAULT = 8;
    constexpr uint64_t VECTOR_PAGE_FAULT = 14;
    constexpr uint64_t VECTOR_MACHINE_CHECK = 18;
    constexpr uint64_t VECTOR_SIMD = 19;
    constexpr uint64_t VECTOR_CONTROL_PROTECTION = 21;
    constexpr uint64_t VECTOR_HV_INJECTION = 28;
    constexpr uint64_t VECTOR_VMM_COMMUNICATION = 29;
    constexpr uint64_t VECTOR_SECURITY = 30;
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

// The vectors below 32 raised for the platform or the kernel and not by the instruction ring 3
// ran: the thread they land on did nothing.
TEST(FaultFrame, AnNmiTakenAtRing3IsNotTheThreads)
{
    EXPECT_FALSE(trap_frame_from_ring3(frame(VECTOR_NMI, CS_USER)));
}

TEST(FaultFrame, ADoubleFaultTakenAtRing3IsNotTheThreads)
{
    EXPECT_FALSE(trap_frame_from_ring3(frame(VECTOR_DOUBLE_FAULT, CS_USER)));
}

TEST(FaultFrame, AMachineCheckTakenAtRing3IsNotTheThreads)
{
    EXPECT_FALSE(trap_frame_from_ring3(frame(VECTOR_MACHINE_CHECK, CS_USER)));
}

TEST(FaultFrame, TheVirtualisationAndSecurityExceptionsAtRing3AreNotTheThreads)
{
    EXPECT_FALSE(trap_frame_from_ring3(frame(VECTOR_HV_INJECTION, CS_USER)));
    EXPECT_FALSE(trap_frame_from_ring3(frame(VECTOR_VMM_COMMUNICATION, CS_USER)));
    EXPECT_FALSE(trap_frame_from_ring3(frame(VECTOR_SECURITY, CS_USER)));
}

// The exclusions are those vectors and no range: their neighbours stay the thread's.
TEST(FaultFrame, TheExceptionsBesideThemStayTheThreads)
{
    EXPECT_TRUE(trap_frame_from_ring3(frame(VECTOR_DIVIDE, CS_USER)));
    EXPECT_TRUE(trap_frame_from_ring3(frame(VECTOR_NMI + 1, CS_USER)));
    EXPECT_TRUE(trap_frame_from_ring3(frame(VECTOR_DOUBLE_FAULT - 1, CS_USER)));
    EXPECT_TRUE(trap_frame_from_ring3(frame(VECTOR_DOUBLE_FAULT + 1, CS_USER)));
    EXPECT_TRUE(trap_frame_from_ring3(frame(VECTOR_MACHINE_CHECK - 1, CS_USER)));
    EXPECT_TRUE(trap_frame_from_ring3(frame(VECTOR_SIMD, CS_USER)));
    EXPECT_TRUE(trap_frame_from_ring3(frame(VECTOR_CONTROL_PROTECTION, CS_USER)));
    EXPECT_TRUE(trap_frame_from_ring3(frame(VECTOR_HV_INJECTION - 1, CS_USER)));
    EXPECT_TRUE(trap_frame_from_ring3(frame(VECTOR_SECURITY + 1, CS_USER)));
}
