// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// An ordered halt halts whatever terminal outcome is set; the software fault reporter ends the
// system through kfault_terminate, which takes it. console_leaves.cc's seam prints each call.

#include <gtest/gtest.h>

#include <kickos/arch/arch.h>
#include <kickos/kernel.h>

#include <unistd.h>

namespace
{
    TEST(ConsoleTerminal, AnOrderedHaltTakesNoTerminalOutcome)
    {
        EXPECT_EXIT(
            {
                dup2(STDERR_FILENO, STDOUT_FILENO);
                kickos_terminate(5);
            },
            ::testing::ExitedWithCode(43), "^SEAM: arch_shutdown\\(5\\)");
    }

    TEST(ConsoleTerminal, TheFaultReporterEndsThroughTheFaultTerminal)
    {
        EXPECT_EXIT(
            {
                dup2(STDERR_FILENO, STDOUT_FILENO);
                kickos_isr_fault(0x1000u, 1);
            },
            ::testing::ExitedWithCode(42), "SEAM: kfault_terminate\n");
    }
}
