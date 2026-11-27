// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A console written outside its ring, once a driver's death reclaimed it or where no ring is
// armed: each wire byte of a thread's line is its own masked span, and an interrupt's kernel line
// arriving inside that line is dropped rather than written into it. A reclaimed console takes no
// user line, so its thread's line is the kernel's.

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

TEST(ConsolePolledLine, AReclaimedConsoleMasksOneByteOfAThreadsLineAtATime)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        publish();
        task_ends();
        ASSERT_EQ(consoleseam::g_reclaims, 1u) << "premise: the console was not reclaimed";
        consoleseam::reset_masked_pushes();
        consoleseam::g_current = thread(1);
        kickos::kputs(kLine.c_str());
        EXPECT_EQ(consoleseam::wire(), kReclaimed + kLine);
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
