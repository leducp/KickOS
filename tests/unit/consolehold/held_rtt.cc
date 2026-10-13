// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// kernel/init/console.cc on a build that also carries RTT: a user line the kernel console is
// offered again, across a dark-window wait or a full ring, reaches RTT exactly once, and only
// once the chip took it.

#include "held_fixture.h"

#if !defined(KICKOS_CONSOLE_RTT) || !KICKOS_CONSOLE_RTT
#error "console_held_rtt is built at the RTT posture, or it witnesses nothing"
#endif

using namespace heldfix;

namespace
{
    constexpr uint32_t kRing = 256u;

    std::string g_rtt;

    // The writer's wait ends with the reclaim, as the last holder's exit lands it.
    int reclaim_in_the_wait(void)
    {
        consoleseam::g_window_free = true;
        kickos::IrqLock lock;
        console_on_driver_death(lock);
        return 0;
    }
}

extern "C" void kickos_rtt_write(char const* buf, size_t n)
{
    g_rtt.append(buf, n);
}

// A user line offered in the dark window waits, then goes out once the reclaim lands: once on
// the wire and once on RTT, not once per offer.
TEST(ConsoleHeldRtt, ADarkWindowWriterReachesRttOnce)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        publish();
        consoleseam::g_window_free = false;
        {
            kickos::IrqLock lock;
            console_note_driver_death();
            console_on_driver_death(lock);
        }
        ASSERT_NE(console_dark(), 0);
        consoleseam::g_current = thread(1);
        consoleseam::g_dark_wait = reclaim_in_the_wait;
        g_rtt.clear();
        EXPECT_EQ(kickos::kconsole_write_user("line\n", 5, true), 5);
        EXPECT_EQ(consoleseam::wire(), "line\n");
        EXPECT_EQ(g_rtt, "line\n") << "each offer put the line on RTT";
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
        EXPECT_EQ(kickos::kconsole_write_user("line\n", 5, false), -KOS_ETIMEDOUT);
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
