// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The lines a chip file gives the kernel's own devices (KICKOS_KERNEL_LINES), refused by the
// real irq.cc while the arch declares nothing of its own.

#include <gtest/gtest.h>

#include "irq_seam.h"

#include <kickos/cap.h>
#include <kickos/irq.h>
#include <kickos/sys/errno.h>
#include <kickos/thread.h>

namespace
{
    constexpr int KERNEL_LINES[] = {KICKOS_KERNEL_LINES};

    kickos::Thread g_claimer;

    void probe_handler(void*)
    {
    }

    struct KernelLines : public ::testing::Test
    {
        void SetUp() override
        {
            kickos::irqfix::reset();
            kickos::irqfix::reset_lock();
            kickos::irqfix::reset_kernel();
            kickos::irqfix::reset_caps();
            kickos::irqfix::g_core = 0;
            g_claimer.affinity = 1u;
        }
    };

    TEST_F(KernelLines, every_listed_line_is_refused_admission)
    {
        for (int const line : KERNEL_LINES)
        {
            EXPECT_EQ(kickos::irq_line_admit(line), -KOS_EPERM)
                << "line " << line << ": the chip file gives it to the kernel and it was admitted";
        }
    }

    TEST_F(KernelLines, a_listed_line_is_refused_by_both_minting_entries)
    {
        uint32_t cap = kickos::KCAP_INVALID;
        EXPECT_FALSE(kickos::irq_attach(KERNEL_LINES[0], probe_handler, nullptr))
            << "irq_attach bound a handler on a line the kernel drives";
        EXPECT_EQ(kickos::irq_claim(&g_claimer, KERNEL_LINES[0], 0u, &cap), -KOS_EPERM);
        EXPECT_EQ(cap, kickos::KCAP_INVALID) << "a refused claim handed back a capability";
        EXPECT_EQ(kickos::irqfix::last_line_op(KERNEL_LINES[0]), -1) << "the refusal reached the controller";
    }

    TEST_F(KernelLines, a_line_the_list_does_not_name_is_admitted)
    {
        EXPECT_EQ(kickos::irq_line_admit(KERNEL_LINES[0] + 1), 0);
        EXPECT_TRUE(kickos::irq_attach(KERNEL_LINES[0] + 1, probe_handler, nullptr));
        kickos::irq_detach(KERNEL_LINES[0] + 1);
    }
}
