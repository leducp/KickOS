// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// sched_admit_mask, the one gate every core mask passes before it is stored: a spawn's
// requested mask, kos_thread_set_affinity, and both halves of kos_task_sched_grant.
//
// Built at four kernel cores with cores 2 and 3 isolated (see CMakeLists.txt), so
// KICKOS_CORE_SET_ALL is 0xf and 0xc is a mask over SEVERAL isolated cores, the shape no board
// in the fleet can express.

#include <gtest/gtest.h>

#include <kickos/sched.h>

#include <kickos/sys/errno.h>

namespace
{
    constexpr uint32_t ALL = 0xfu;  // KICKOS_CORE_SET_ALL at four cores
    constexpr uint32_t ISO = 0xcu;  // KICKOS_ISOLATED_CORES
    constexpr uint32_t FREE = 0x3u; // the cores a thread that names none is given

    using kickos::MaskBound;
    using kickos::sched_admit_mask;
    using kickos::sched_placeable_on;
    using kickos::Thread;

    unsigned cores_taking(uint32_t mask)
    {
        Thread t{};
        t.affinity = mask;
        unsigned n = 0;
        for (uint32_t core = 0; core < KICKOS_KERNEL_CORES; core++)
        {
            if (sched_placeable_on(&t, core))
            {
                n++;
            }
        }
        return n;
    }

    // The two constants the whole file is written against, so a preset change that moved them
    // fails here rather than turning every arm below into a different question.
    TEST(AdmitMask, TheBuildCarriesTheShapeTheseArmsAssume)
    {
        EXPECT_EQ(static_cast<uint32_t>(KICKOS_CORE_SET_ALL), ALL);
        EXPECT_EQ(static_cast<uint32_t>(KICKOS_ISOLATED_CORES), ISO);
        EXPECT_EQ(ALL & ~ISO, FREE);
    }

    // EVERY mask this kernel can express, named as a SET and asked of both disciplines. A
    // single sample would pass for a rule that refused some other mask instead.
    TEST(AdmitMask, EveryMaskInsideTheGrantIsAdmittedWhole)
    {
        for (uint32_t m = 1; m <= ALL; m++)
        {
            uint32_t effective = 0;
            EXPECT_EQ(sched_admit_mask(m, ALL, MaskBound::INTERSECT, &effective), 0) << m;
            EXPECT_EQ(effective, m) << m;

            uint32_t granted = 0;
            EXPECT_EQ(sched_admit_mask(m, ALL, MaskBound::SUBSET, &granted), 0) << m;
            EXPECT_EQ(granted, m) << m;
        }
    }

    // A mask over SEVERAL isolated cores, which is what an explicit affinity hands a reserved
    // pool. Every bit of it takes the thread, so the count is the whole answer: a rule that
    // seated it on one of them, or on none, reads differently here.
    TEST(AdmitMask, EveryBitOfAMultiIsolatedMaskTakesTheThread)
    {
        EXPECT_EQ(cores_taking(ISO), 2u);
        EXPECT_EQ(cores_taking(ALL), 4u);
        EXPECT_EQ(cores_taking(FREE), 2u);
    }

    // A grant of ONE isolated core, which is the pool of one and the shape the fleet's isolated
    // posture carries.
    TEST(AdmitMask, OneIsolatedCoreIsAdmittedAsAPin)
    {
        uint32_t effective = 0;
        EXPECT_EQ(sched_admit_mask(0x8u, ALL, MaskBound::INTERSECT, &effective), 0);
        EXPECT_EQ(effective, 0x8u);

        uint32_t granted = 0;
        EXPECT_EQ(sched_admit_mask(0x8u, ALL, MaskBound::SUBSET, &granted), 0);
        EXPECT_EQ(granted, 0x8u);

        EXPECT_EQ(cores_taking(0x8u), 1u);
    }

    // Naming no core this kernel schedules is MALFORMED and not an authority answer: no grant
    // on any board could satisfy it.
    TEST(AdmitMask, MaskNamingOnlyUndrivenCoresIsMalformed)
    {
        uint32_t effective = 0;
        EXPECT_EQ(sched_admit_mask(1u << 4, ALL, MaskBound::INTERSECT, &effective),
                  -KOS_EINVAL);
        EXPECT_EQ(sched_admit_mask(1u << 4, ALL, MaskBound::SUBSET, &effective), -KOS_EINVAL);
    }

    // THE MACHINE BEFORE THE GRANT. A bit naming a core this kernel does not schedule is
    // dropped rather than refused, which is what keeps all ones an ordinary request for the
    // whole grant rather than a mask no board could satisfy.
    TEST(AdmitMask, UndrivenBitsAreDroppedNotRefused)
    {
        uint32_t effective = 0;
        EXPECT_EQ(sched_admit_mask(~0u, FREE, MaskBound::INTERSECT, &effective), 0);
        EXPECT_EQ(effective, FREE);

        // The same request against a task's grant, where the answer used to depend on which of
        // the two paths asked it.
        uint32_t granted = 0;
        EXPECT_EQ(sched_admit_mask(~0u, ALL, MaskBound::SUBSET, &granted), 0);
        EXPECT_EQ(granted, ALL);
    }

    // A zero word means a different thing at each ABI that carries one: the task's default
    // set at a spawn and at kos_thread_set_affinity, "leave this half alone" in a grant. So
    // the admission treats it as the empty set and every caller resolves its own meaning
    // before calling.
    TEST(AdmitMask, EmptyMaskIsMalformed)
    {
        uint32_t effective = 0;
        EXPECT_EQ(sched_admit_mask(0, FREE, MaskBound::INTERSECT, &effective), -KOS_EINVAL);
        EXPECT_EQ(sched_admit_mask(0, FREE, MaskBound::SUBSET, &effective), -KOS_EINVAL);
    }

    // A THREAD's mask is a set of ACCEPTABLE cores, so the grant intersects it.
    TEST(AdmitMask, IntersectNarrowsToTheGrantAndRefusesAnEmptyMeet)
    {
        uint32_t effective = 0;
        EXPECT_EQ(sched_admit_mask(0x3u, 0x1u, MaskBound::INTERSECT, &effective), 0);
        EXPECT_EQ(effective, 0x1u);
        EXPECT_EQ(sched_admit_mask(0x2u, 0x1u, MaskBound::INTERSECT, &effective), -KOS_EPERM);
    }

    // A TASK's grant is an AUTHORITY, so the same request that a thread's mask narrows to is
    // refused here and never clamped.
    TEST(AdmitMask, SubsetRefusesAWideningInsteadOfClampingIt)
    {
        uint32_t granted = 0;
        EXPECT_EQ(sched_admit_mask(0x3u, 0x1u, MaskBound::SUBSET, &granted), -KOS_EPERM);
        EXPECT_EQ(sched_admit_mask(0x1u, 0x1u, MaskBound::SUBSET, &granted), 0);
        EXPECT_EQ(granted, 0x1u);
    }
}
