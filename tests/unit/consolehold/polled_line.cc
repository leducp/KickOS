// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A console written outside its ring, once a driver's death reclaimed it or where no ring is
// armed: each wire byte of a task's line is its own masked span, and an interrupt's kernel line
// arriving inside that line is dropped rather than written into it.

#include "held_fixture.h"

using namespace heldfix;

namespace
{
    constexpr uint32_t kRing = 256u;
    std::string const kLine = "a task's line on a polled console\n";

    void write_user_line()
    {
        consoleseam::g_current = thread(1);
        EXPECT_EQ(kickos::kconsole_write_user(kLine.data(), kLine.size(), true),
                  static_cast<int>(kLine.size()));
    }
}

TEST(ConsolePolledLine, AReclaimedConsoleMasksOneByteOfATasksLineAtATime)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        publish();
        task_ends();
        ASSERT_EQ(consoleseam::g_reclaims, 1u) << "premise: the console was not reclaimed";
        ASSERT_LE(consoleseam::max_masked_pushes(), 1u) << "premise: a wide span before the line";
        write_user_line();
        EXPECT_EQ(consoleseam::wire(), kLine);
        EXPECT_EQ(consoleseam::max_masked_pushes(), 1u)
            << "a masked span carried more than one byte of the task's line";
    });
}

TEST(ConsolePolledLine, AnUnarmedRingMasksOneByteOfATasksLineAtATime)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        console_tx_disarm();
        write_user_line();
        EXPECT_EQ(consoleseam::wire(), kLine);
        EXPECT_EQ(consoleseam::max_masked_pushes(), 1u)
            << "a masked span carried more than one byte of the task's line";
    });
}

TEST(ConsolePolledLine, AnInterruptsLineInsideATasksLineIsDropped)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        console_tx_disarm();
        consoleseam::run_in_gap(consoleseam::gap_count() + 8u, []() {
            ringleaves::g_in_isr = true;
            kickos::kprintf("isr line\n");
            ringleaves::g_in_isr = false;
        });
        write_user_line();
        ASSERT_TRUE(consoleseam::seat_fired()) << "premise: no interrupt landed inside the line";
        EXPECT_EQ(consoleseam::wire(), kLine) << "the interrupt's line landed inside the task's";
        kickos::kprintf("isr line\n");
        EXPECT_EQ(consoleseam::wire(), kLine + "isr line\n");
    });
}
