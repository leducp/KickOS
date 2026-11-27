// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// kernel/init/console.cc on a build that also carries RTT: a user line reaches RTT only once the
// chip took it, so a line a full ring refuses is not there yet, and a line a published or
// reclaimed console refuses never is.

#include "held_fixture.h"

#if !defined(KICKOS_CONSOLE_RTT) || !KICKOS_CONSOLE_RTT
#error "console_held_rtt is built at the RTT posture, or it witnesses nothing"
#endif

using namespace heldfix;

namespace
{
    constexpr uint32_t kRing = 256u;

    std::string g_rtt;
}

extern "C" void kickos_rtt_write(char const* buf, size_t n)
{
    g_rtt.append(buf, n);
}

// A user line once a console is published, its driver alive, dead or reclaimed, reaches neither
// the wire nor RTT.
TEST(ConsoleHeldRtt, APublishedConsoleKeepsUserLinesOffRtt)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        publish();
        consoleseam::g_current = thread(1);
        g_rtt.clear();
        EXPECT_EQ(kickos::kconsole_write_user("live\n", 5, true), -KOS_EBUSY);
        consoleseam::g_window_free = false;
        {
            kickos::IrqLock lock;
            console_note_driver_death();
            console_on_driver_death();
        }
        EXPECT_EQ(kickos::kconsole_write_user("dead\n", 5, true), -KOS_EBUSY);
        consoleseam::g_window_free = true;
        {
            kickos::IrqLock lock;
            console_on_driver_death();
        }
        ASSERT_EQ(consoleseam::g_reclaims, 1u);
        EXPECT_EQ(kickos::kconsole_write_user("back\n", 5, true), -KOS_EBUSY);
        EXPECT_EQ(g_rtt, kReclaimed) << "a user line reached RTT past the publish";
        EXPECT_EQ(consoleseam::wire(), kReclaimed);
    });
}

// A user line the full ring refuses is not on RTT: it goes there when the chip takes it.
TEST(ConsoleHeldRtt, ARefusedUserLineIsNotOnRtt)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        consoleseam::set_slot_free(0);
        consoleseam::g_current = thread(1);
        std::string const fill(1u, 'f');
        while (kickos::kconsole_write_user(fill.data(), fill.size(), false) > 0)
        {
        }
        g_rtt.clear();
        EXPECT_EQ(kickos::kconsole_write_user("line\n", 5, false), -KOS_EAGAIN);
        EXPECT_EQ(g_rtt, "") << "a line the console refused reached RTT";
    });
}

// The control: a kernel line is offered once, and RTT carries it whatever the chip did with it.
TEST(ConsoleHeldRtt, AKernelLineReachesRttUnderADriver)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        publish();
        g_rtt.clear();
        kickos::kputs("kernel\n");
        EXPECT_EQ(g_rtt, "kernel\n");
        EXPECT_EQ(consoleseam::pushes_after_commit(), 0u);
    });
}

// A refusal under no lock reaches RTT as well as the chip.
TEST(ConsoleHeldRtt, AnUnlockedRefusalReachesRtt)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        g_rtt.clear();
        kpanic_enter_unlocked();
        kpanic_write_unlocked("refused\n", 8);
        EXPECT_EQ(g_rtt, "refused\n") << "an RTT console printed no refusal";
    });
}
