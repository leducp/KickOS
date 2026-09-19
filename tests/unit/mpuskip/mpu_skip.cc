// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The PMSAv7 commit's per-descriptor skip, over the SHIPPING backend
// (arch/arm/common/arch_arm_mpu_pmsav7.cc) and a modelled MPU it writes through the real
// register addresses.
//
// What only this gate can see: the record of what the hardware holds is file-static inside
// the backend, so nothing on target can ask which descriptors a commit wrote. The arms that
// matter are the two the skip could get wrong and no board test would notice: a set whose
// WORDS changed under an unchanged image pointer, and a commit after an invalidation.

#include "arm_regs_seam.h"

#include <kickos/arch/arch.h>

#include <gtest/gtest.h>

extern "C"
{
    void kickos_arm_mpu_program(struct arch_mpu_encoded const* img);
    void kickos_arm_mpu_fixed_init(void);
}

namespace
{
    using kickos::testfix::g_mpu;

    // Two sets of one domain, differing in the LAST-APPENDED slot alone: the thread stack,
    // which kernel/thread/thread.cc appends last. This is the shape the measurement behind
    // the skip found in every near miss.
    void seat_domain(struct arch_mpu_encoded* img, uint32_t stack_base)
    {
        for (size_t i = 0; i < ARCH_MPU_ENCODED_SLOTS; i++)
        {
            img->rbar[i] = 0x20000000u + static_cast<uint32_t>(i) * 0x1000u;
            img->rasr[i] = 0x03000013u + static_cast<uint32_t>(i);
        }
        img->rbar[ARCH_MPU_ENCODED_SLOTS - 1] = stack_base;
    }

    unsigned descriptors_touched(size_t first, size_t last)
    {
        unsigned n = 0;
        for (size_t i = first; i < last; i++)
        {
            if (g_mpu.desc[i].touches != 0)
            {
                n++;
            }
        }
        return n;
    }

    // A fresh backend record cannot be asked for directly: the flag is file-static. Every
    // case therefore opens by driving the one site that clears it, which is also the site a
    // chip with fixed regions calls at boot.
    void invalidate_record()
    {
        kickos_arm_mpu_fixed_init();
    }

    class MpuSkip : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            kickos::testfix::mpu_reset(8);
            kickos::testfix::mpu_set_fixed(nullptr, 0);
            invalidate_record();
            kickos::testfix::mpu_clear_trace();
        }
    };
}

// The first commit after an invalidation is TOTAL: every descriptor carries the image and
// the MPU is enabled. Without this the skip arms below would pass vacuously on a commit
// that programmed nothing at all.
TEST_F(MpuSkip, a_total_commit_programs_every_descriptor)
{
    struct arch_mpu_encoded a = {};
    seat_domain(&a, 0x20008000u);

    kickos_arm_mpu_program(&a);

    EXPECT_EQ(descriptors_touched(0, 8), 8u);
    for (size_t i = 0; i < ARCH_MPU_ENCODED_SLOTS; i++)
    {
        EXPECT_EQ(g_mpu.desc[i].rbar, a.rbar[i]) << "slot " << i;
        EXPECT_EQ(g_mpu.desc[i].rasr, a.rasr[i]) << "slot " << i;
    }
    EXPECT_NE(g_mpu.ctrl & 1u, 0u); // MPU_CTRL_ENABLE
    EXPECT_EQ(g_mpu.unknown_reads, 0u);
}

// The whole point: a switch between two threads of one domain rewrites the stack descriptor
// and leaves the other seven standing.
TEST_F(MpuSkip, a_switch_within_a_domain_writes_one_descriptor)
{
    struct arch_mpu_encoded a = {};
    struct arch_mpu_encoded b = {};
    seat_domain(&a, 0x20008000u);
    seat_domain(&b, 0x2000C000u);

    kickos_arm_mpu_program(&a);
    kickos::testfix::mpu_clear_trace();
    kickos_arm_mpu_program(&b);

    EXPECT_EQ(descriptors_touched(0, 8), 1u);
    EXPECT_NE(g_mpu.desc[ARCH_MPU_ENCODED_SLOTS - 1].touches, 0u);
    for (size_t i = 0; i < ARCH_MPU_ENCODED_SLOTS; i++)
    {
        EXPECT_EQ(g_mpu.desc[i].rbar, b.rbar[i]) << "slot " << i;
        EXPECT_EQ(g_mpu.desc[i].rasr, b.rasr[i]) << "slot " << i;
    }
}

// A commit whose image already stands writes no descriptor and issues no barrier: there is
// nothing to synchronise.
TEST_F(MpuSkip, an_unchanged_set_writes_nothing)
{
    struct arch_mpu_encoded a = {};
    seat_domain(&a, 0x20008000u);

    kickos_arm_mpu_program(&a);
    kickos::testfix::mpu_clear_trace();
    kickos_arm_mpu_program(&a);

    EXPECT_EQ(descriptors_touched(0, 16), 0u);
    EXPECT_EQ(g_mpu.dsb, 0u);
    EXPECT_EQ(g_mpu.isb, 0u);
}

// THE CHEAP FORM IS UNSOUND, and this is the arm that says so. MpuSet's mutators re-encode
// in place and thread slots come from a static pool, so the same image address can hold
// different words on a later commit: a commit comparing POINTERS would skip this one.
TEST_F(MpuSkip, the_same_image_with_changed_words_is_programmed)
{
    struct arch_mpu_encoded a = {};
    seat_domain(&a, 0x20008000u);
    kickos_arm_mpu_program(&a);

    a.rasr[2] |= 0x10000000u; // the XN a retype adds, written into the image in place
    kickos::testfix::mpu_clear_trace();
    kickos_arm_mpu_program(&a);

    EXPECT_EQ(descriptors_touched(0, 8), 1u);
    EXPECT_EQ(g_mpu.desc[2].rasr, a.rasr[2]);
}

// A partial commit makes a record/hardware disagreement PERMANENT where a total commit
// erased it, so a descriptor the record says is current must be reprogrammed once the
// record is invalidated. The hardware is zeroed behind the backend's back to stand for the
// descriptors an outside writer moved.
TEST_F(MpuSkip, an_invalidation_makes_the_next_commit_total)
{
    struct arch_mpu_encoded a = {};
    struct arch_mpu_encoded b = {};
    seat_domain(&a, 0x20008000u);
    seat_domain(&b, 0x2000C000u);
    kickos_arm_mpu_program(&a);
    kickos_arm_mpu_program(&b);

    for (size_t i = 0; i < kickos::testfix::MPU_HW_SLOTS; i++)
    {
        g_mpu.desc[i].rbar = 0;
        g_mpu.desc[i].rasr = 0;
    }
    invalidate_record();
    kickos::testfix::mpu_clear_trace();
    kickos_arm_mpu_program(&b);

    EXPECT_EQ(descriptors_touched(0, 8), 8u);
    for (size_t i = 0; i < ARCH_MPU_ENCODED_SLOTS; i++)
    {
        EXPECT_EQ(g_mpu.desc[i].rbar, b.rbar[i]) << "slot " << i;
        EXPECT_EQ(g_mpu.desc[i].rasr, b.rasr[i]) << "slot " << i;
    }
    EXPECT_NE(g_mpu.ctrl & 1u, 0u);
}

// Without this the arm above proves only that SOMETHING was written: the record has to be
// what decides, not the words happening to differ from a zeroed descriptor.
TEST_F(MpuSkip, without_the_invalidation_the_zeroed_descriptors_stay_zeroed)
{
    struct arch_mpu_encoded a = {};
    struct arch_mpu_encoded b = {};
    seat_domain(&a, 0x20008000u);
    seat_domain(&b, 0x2000C000u);
    kickos_arm_mpu_program(&a);
    kickos_arm_mpu_program(&b);

    for (size_t i = 0; i < kickos::testfix::MPU_HW_SLOTS; i++)
    {
        g_mpu.desc[i].rbar = 0;
        g_mpu.desc[i].rasr = 0;
    }
    kickos::testfix::mpu_clear_trace();
    kickos_arm_mpu_program(&b);

    EXPECT_EQ(descriptors_touched(0, 8), 0u);
    EXPECT_EQ(g_mpu.desc[0].rbar, 0u);
}

// Chip fixed rows own the LOW slots and NO commit may write one, whatever the record says:
// on imxrt1062 those carry the anti-speculation wrap over the band this code runs from.
TEST_F(MpuSkip, a_commit_never_reaches_a_chip_fixed_row)
{
    static kickos_arm_mpu_fixed_region const rows[2] = {{0x60000000u, 0x0300003Fu},
                                                        {0x70000000u, 0x0300003Du}};
    kickos::testfix::mpu_reset(16);
    kickos::testfix::mpu_set_fixed(rows, 2);
    invalidate_record();
    kickos::testfix::mpu_clear_trace();

    struct arch_mpu_encoded a = {};
    struct arch_mpu_encoded b = {};
    seat_domain(&a, 0x20008000u);
    seat_domain(&b, 0x2000C000u);
    kickos_arm_mpu_program(&a);
    kickos_arm_mpu_program(&b);

    EXPECT_EQ(g_mpu.desc[0].touches, 0u);
    EXPECT_EQ(g_mpu.desc[1].touches, 0u);
    EXPECT_EQ(g_mpu.desc[0].rbar, rows[0].base);
    EXPECT_EQ(g_mpu.desc[1].rbar, rows[1].base);
    // Per-thread grants sit ABOVE the fixed rows, so the stack descriptor is slot 2 + 7.
    EXPECT_EQ(g_mpu.desc[2 + ARCH_MPU_ENCODED_SLOTS - 1].rbar, b.rbar[ARCH_MPU_ENCODED_SLOTS - 1]);
}

// A part implementing more descriptors than the image fills has its tail disabled by the
// total commit, and no later commit spends a write on it.
TEST_F(MpuSkip, the_tail_past_the_image_is_disabled_once)
{
    kickos::testfix::mpu_reset(16);
    for (size_t i = 0; i < kickos::testfix::MPU_HW_SLOTS; i++)
    {
        g_mpu.desc[i].rasr = 0xFFFFFFFFu; // stale enables, as a reset part would not have
    }
    invalidate_record();
    kickos::testfix::mpu_clear_trace();

    struct arch_mpu_encoded a = {};
    struct arch_mpu_encoded b = {};
    seat_domain(&a, 0x20008000u);
    seat_domain(&b, 0x2000C000u);

    kickos_arm_mpu_program(&a);
    for (size_t i = ARCH_MPU_ENCODED_SLOTS; i < kickos::testfix::MPU_HW_SLOTS; i++)
    {
        EXPECT_EQ(g_mpu.desc[i].rasr, 0u) << "slot " << i;
    }
    kickos::testfix::mpu_clear_trace();
    kickos_arm_mpu_program(&b);
    EXPECT_EQ(descriptors_touched(ARCH_MPU_ENCODED_SLOTS, kickos::testfix::MPU_HW_SLOTS), 0u);
}

// A null stash is a commit with nothing to program and must not disturb the record: the
// commit path returns on it before any descriptor is read.
TEST_F(MpuSkip, a_null_image_leaves_the_record_alone)
{
    struct arch_mpu_encoded a = {};
    seat_domain(&a, 0x20008000u);
    kickos_arm_mpu_program(&a);

    kickos::testfix::mpu_clear_trace();
    kickos_arm_mpu_program(nullptr);
    EXPECT_EQ(descriptors_touched(0, 16), 0u);

    kickos_arm_mpu_program(&a);
    EXPECT_EQ(descriptors_touched(0, 16), 0u);
}
