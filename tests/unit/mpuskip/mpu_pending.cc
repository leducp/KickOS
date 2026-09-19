// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The deferred-commit STASH, over the shipping ARM member that owns it
// (arch/arm/common/arch_arm_mpu_pending.cc) and the shipping PMSAv7 writer it feeds, with
// the PendSV epilogue modelled by the seam's kickos_arch_mpu_commit.
//
// What only this gate can see: the stash is one file-static cell, and nothing on target can
// ask what it holds. The arm that matters is the one no board test would notice: a grant
// committed while a switch to ANOTHER thread is already pended, where the epilogue would
// otherwise program the granting thread's descriptors onto the incoming one and leave them
// standing until the next switch re-stashed.

#include "arm_regs_seam.h"

#include <kickos/arch/arch.h>

#include <gtest/gtest.h>

extern "C"
{
    void kickos_arm_mpu_program(struct arch_mpu_encoded const* img);
    void kickos_arm_mpu_fixed_init(void);
    struct arch_mpu_encoded const* kickos_arm_mpu_pending(void);
}

namespace
{
    using kickos::testfix::g_mpu;

    // Every slot distinct per set, so a set programmed onto the wrong thread is visible in
    // any descriptor and not only in the last-appended one.
    void seat_set(struct arch_mpu_encoded* img, uint32_t base)
    {
        for (size_t i = 0; i < ARCH_MPU_ENCODED_SLOTS; i++)
        {
            img->rbar[i] = base + static_cast<uint32_t>(i) * 0x1000u;
            img->rasr[i] = 0x03000013u + static_cast<uint32_t>(i);
        }
    }

    void expect_hardware_holds(struct arch_mpu_encoded const& img)
    {
        for (size_t i = 0; i < ARCH_MPU_ENCODED_SLOTS; i++)
        {
            EXPECT_EQ(g_mpu.desc[i].rbar, img.rbar[i]) << "slot " << i;
            EXPECT_EQ(g_mpu.desc[i].rasr, img.rasr[i]) << "slot " << i;
        }
    }

    unsigned descriptors_touched()
    {
        unsigned n = 0;
        for (size_t i = 0; i < ARCH_MPU_ENCODED_SLOTS; i++)
        {
            if (g_mpu.desc[i].touches != 0)
            {
                n++;
            }
        }
        return n;
    }

    class MpuPending : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            kickos::testfix::mpu_reset(8);
            kickos::testfix::mpu_set_fixed(nullptr, 0);
            // Both file-statics survive the case that set them: the backend's record is put
            // back by the one site that clears it, and the stash by the only expression that
            // empties it, which is also the state it boots in.
            kickos_arm_mpu_fixed_init();
            arch_mpu_apply(nullptr, 0, nullptr);
            kickos::testfix::mpu_clear_trace();
        }
    };
}

// THE REGRESSION. A switch to another thread is booked, then the caller self-grants: the
// grant must be live when the syscall returns AND the epilogue must still program the thread
// the switch lands on. Committing through arch_mpu_apply plus kickos_arch_mpu_commit passes
// the first half and fails the second.
TEST_F(MpuPending, a_booked_switch_keeps_its_own_image_across_a_self_grant)
{
    struct arch_mpu_encoded incoming = {};
    struct arch_mpu_encoded caller = {};
    seat_set(&incoming, 0x20040000u);
    seat_set(&caller, 0x20080000u);

    // The switch DECISION (kernel/sched/sched.cc switch_book): stash only, no write.
    arch_mpu_apply(nullptr, 0, &incoming);
    EXPECT_EQ(descriptors_touched(), 0u);

    // The self-grant (kernel/syscall/syscall.cc, KOS_SYS_MEM_SELF_GRANT).
    arch_mpu_apply_now(nullptr, 0, &caller);
    expect_hardware_holds(caller); // the grant is effective before the syscall returns
    EXPECT_EQ(kickos_arm_mpu_pending(), &incoming);

    // The switch EPILOGUE, after the physical swap.
    kickos_arch_mpu_commit();
    expect_hardware_holds(incoming);
    EXPECT_EQ(g_mpu.unknown_reads, 0u);
}

// The stash swap is masked. Without the bracket an interrupt between the two writes could
// book a switch whose image the restore would then throw away.
TEST_F(MpuPending, a_self_grant_commits_inside_its_own_interrupt_bracket)
{
    struct arch_mpu_encoded caller = {};
    seat_set(&caller, 0x20080000u);

    arch_mpu_apply_now(nullptr, 0, &caller);

    EXPECT_GT(g_mpu.commit_irq_depth, 0u);
    EXPECT_EQ(g_mpu.irq_depth, 0u); // and the restore put back what the save found
}

// The bracket arm above proves nothing unless the seam can read an UNmasked commit, which is
// what the switch epilogue is.
TEST_F(MpuPending, a_switch_epilogue_commit_carries_no_bracket_of_its_own)
{
    struct arch_mpu_encoded incoming = {};
    seat_set(&incoming, 0x20040000u);

    arch_mpu_apply(nullptr, 0, &incoming);
    kickos_arch_mpu_commit();

    EXPECT_EQ(g_mpu.commit_irq_depth, 0u);
}

// NO SWITCH PENDED, which is every self-grant on a quiet system: the grant lands, and the
// stash is left as empty as it was, so a later epilogue programs nothing rather than
// reinstating the granting thread's set on whoever runs next.
TEST_F(MpuPending, a_self_grant_with_nothing_booked_leaves_the_stash_empty)
{
    struct arch_mpu_encoded caller = {};
    seat_set(&caller, 0x20080000u);

    arch_mpu_apply_now(nullptr, 0, &caller);
    expect_hardware_holds(caller);
    EXPECT_EQ(kickos_arm_mpu_pending(), nullptr);

    kickos::testfix::mpu_clear_trace();
    kickos_arch_mpu_commit();
    EXPECT_EQ(descriptors_touched(), 0u);
}

// The steady state: the stash names the CALLER, its own switch-in having put it there. The
// restore must leave the record and the stash agreeing with the hardware, so the next
// epilogue writes nothing, the widened descriptor included.
TEST_F(MpuPending, a_self_grant_over_the_callers_own_stash_moves_one_descriptor)
{
    struct arch_mpu_encoded caller = {};
    seat_set(&caller, 0x20080000u);

    arch_mpu_apply(nullptr, 0, &caller); // the caller's own switch-in
    kickos_arch_mpu_commit();
    kickos::testfix::mpu_clear_trace();

    // MpuSet's mutators re-encode IN PLACE, so a grant widens the words under an unchanged
    // pointer. That is the case the record exists for.
    caller.rbar[3] = 0x20300000u;
    arch_mpu_apply_now(nullptr, 0, &caller);

    EXPECT_EQ(kickos_arm_mpu_pending(), &caller);
    EXPECT_EQ(descriptors_touched(), 1u);
    EXPECT_EQ(g_mpu.desc[3].rbar, 0x20300000u);

    kickos::testfix::mpu_clear_trace();
    kickos_arch_mpu_commit();
    EXPECT_EQ(descriptors_touched(), 0u);
}
