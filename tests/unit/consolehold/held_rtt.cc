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
        heldseam::g_window_free = true;
        kickos::IrqLock lock;
        console_on_driver_death();
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
        heldseam::g_window_free = false;
        {
            kickos::IrqLock lock;
            console_note_driver_death();
            console_on_driver_death();
        }
        ASSERT_NE(console_dark(), 0);
        heldseam::g_current = reinterpret_cast<kickos::Thread*>(0x100u);
        heldseam::g_dark_wait = reclaim_in_the_wait;
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
        heldseam::g_current = reinterpret_cast<kickos::Thread*>(0x100u);
        std::string const fill(1u, 'f');
        while (kickos::kconsole_write_user(fill.data(), fill.size(), false) != 0)
        {
        }
        g_rtt.clear();
        EXPECT_EQ(kickos::kconsole_write_user("line\n", 5, false), 0);
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
