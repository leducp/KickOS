// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// kernel/init/console.cc and kernel/init/console_tx.cc against a panic landing between a writer's
// ownership read and its insert, which above one core is another core's panic.

#include <gtest/gtest.h>

#include <string>

#include <kickos/console_tx.h>
#include <kickos/kernel.h>

#include "console_seam.h"
#include "isolated_arm.h"

namespace
{
    constexpr uint32_t kRing = 128u;

    // The RTT arm takes an IrqLock of its own ahead of the chip path, one gap more.
#if defined(KICKOS_CONSOLE_RTT) && KICKOS_CONSOLE_RTT
    constexpr uint32_t kGapBase = 1u;
#else
    constexpr uint32_t kGapBase = 0u;
#endif

    void panic_and_print(void)
    {
        kpanic_enter();
        kickos::kputs("PANIC\n");
    }
}

// A line queued before the panic goes out ahead of its banner, and a writer that read
// KERNEL_OWNED before it lands its line after the banner, whole.
TEST(ConsolePanicRace, ALineInsertedAfterThePanicsFlushGoesOutPolledAndWhole)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        consoleseam::set_isr_runs_in_gap(false);
        kickos::kputs("queued\n");
        // The next gap closes console_emit's state-read-plus-count bracket, before the insert
        // reads `armed`.
        consoleseam::run_in_gap(consoleseam::gap_count() + kGapBase + 1u, panic_and_print);
        kickos::kputs("late line\n");
        ASSERT_TRUE(consoleseam::seat_fired()) << "the panic never fired: no race was run";
        EXPECT_EQ(consoleseam::wire(), "queued\nPANIC\nlate line\n")
            << "a line inserted after the panic's flush stayed in a ring nothing drains";
    });
}

namespace
{
    int g_counted_in_panic = -1;
}

// A panic's own lines are not counted: no publish can follow it, so nothing drains them.
TEST(ConsolePanicRace, APanicsLinesAreNotCounted)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        kpanic_enter();
        consoleseam::run_in_sync_write(1u, []() { g_counted_in_panic = console_chip_writers(); });
        kickos::kputs("PANIC\n");
        EXPECT_EQ(consoleseam::wire(), "PANIC\n");
        EXPECT_EQ(g_counted_in_panic, 0) << "a panic's line was counted as a chip writer";
    });
}
