// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The kernel console's polled line writer at a CRLF console's posture, over a channel that
// stalls.

#include <gtest/gtest.h>

#include <kickos/arch/arch.h>
#include <kickos/cap.h>
#include <kickos/console_tx.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sys/errno.h>

#include <stdio.h>
#include <stdlib.h>

#include <string>

namespace
{
    std::string g_wire;
    int g_sync_calls = 0;
    bool g_stalled = false;
    kickos::Thread g_writer;
}

extern "C"
{
    arch_irq_state_t arch_irq_save(void) { return 0; }
    void arch_irq_restore(arch_irq_state_t) {}

    int arch_console_write(char const*, size_t n) { return static_cast<int>(n); }
    bool arch_console_write_sync(char const* buf, size_t n)
    {
        g_sync_calls = g_sync_calls + 1;
        if (g_stalled)
        {
            return false;
        }
        g_wire.append(buf, n);
        return true;
    }
    void arch_console_reclaim(void) {}

    int console_tx_armed(void) { return 0; }
    void console_tx_deinit(void) {}
}

namespace kickos
{
    bool dev_window_free(uintptr_t, size_t) { return true; }

    int console_dark_wait(void) { return -KOS_ECANCELED; }
    void console_dark_wake(Held) {}

    bool cap_console_serves(Thread const*) { return false; }

    namespace sched
    {
        Thread* current() { return &g_writer; }
    }
}

namespace
{
    TEST(ConsoleLineSync, ALiveChannelTakesEverySegmentOfTheLine)
    {
        g_wire.clear();
        g_sync_calls = 0;
        g_stalled = false;
        console_write_line_sync("a\nb\nc", 5);
        EXPECT_EQ(g_wire, "a\r\nb\r\nc");
    }

    TEST(ConsoleLineSync, AStalledChannelCostsOneStallPerLine)
    {
        g_wire.clear();
        g_sync_calls = 0;
        g_stalled = true;
        console_write_line_sync("a\nb\nc\n", 6);
        EXPECT_EQ(g_sync_calls, 1);
    }
}
