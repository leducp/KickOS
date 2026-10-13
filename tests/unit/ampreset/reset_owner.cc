// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Built twice, as node 0 (the reset owner) and as node 1. A case setting the terminal outcome
// runs in a child process: it is set once per process.

#include <gtest/gtest.h>

#include <kickos/cap.h>
#include <kickos/kernel.h>

#include <kickos/sys/abi.h>

#include <stdio.h>
#include <stdlib.h>

#include "reset_seam.h"

namespace
{
    using namespace kickos;

    constexpr bool OWNER = KICKOS_AMP_NODE_ID == KICKOS_AMP_RESET_OWNER;

    // Set `outcome` with both authorities, apply it, and report what reached the chip.
    void set_and_apply(uint32_t outcome)
    {
        resetfix::g_authority = AUTH_SYSTEM | AUTH_BOOTLOADER;
        int const rc = terminal_outcome_set(nullptr, outcome);
        kickos_terminal_outcome_apply();
        fprintf(stderr, "set %d reboot %d\n", rc, static_cast<int>(resetfix::g_reboot_outcome));
        exit(0);
    }

    TEST(AmpReset, OnlyTheOwnerOwnsTheReset)
    {
        EXPECT_EQ(reset_owned_here(), OWNER);
    }

    TEST(AmpReset, ABootloaderOutcomeIsTheOwnersAlone)
    {
        char const* expect = "set -1 reboot -1";
        if (OWNER)
        {
            expect = "set 0 reboot 2";
        }
        EXPECT_EXIT(set_and_apply(KOS_SHUTDOWN_BOOTLOADER), ::testing::ExitedWithCode(0), expect);
    }

    TEST(AmpReset, ARebootOutcomeIsTheOwnersAlone)
    {
        char const* expect = "set -1 reboot -1";
        if (OWNER)
        {
            expect = "set 0 reboot 1";
        }
        EXPECT_EXIT(set_and_apply(KOS_SHUTDOWN_REBOOT), ::testing::ExitedWithCode(0), expect);
    }

    TEST(AmpReset, AHaltOutcomeIsEveryNodesOwn)
    {
        EXPECT_EXIT(set_and_apply(KOS_SHUTDOWN_HALT), ::testing::ExitedWithCode(0),
                    "set 0 reboot -1");
    }
}
