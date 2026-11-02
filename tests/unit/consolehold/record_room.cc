// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A thread-fault record on the kernel's own console behind a full ring: its line is refused
// whole, as any line is, and nothing of it or of the queued lines is written at the device by
// the refusal. The waiting is the caller's (kconsole_record_wait, room_wait.cc).

#include "held_fixture.h"

using namespace heldfix;

namespace
{
    constexpr uint32_t kRing = 256u;

    // Ordinary lines until the ring refuses one, with no drain running: what the ring holds.
    std::string fill()
    {
        std::string queued;
        for (char c = 'a'; c <= 'z'; c++)
        {
            std::string const line = std::string(40, c) + "\n";
            if (console_tx_insert_line(line.data(), line.size(), KICKOS_CONSOLE_CRLF) == 0)
            {
                break;
            }
            queued += line;
        }
        return queued;
    }

    std::string head(char const* who)
    {
        char b[96];
        snprintf(b, sizeof(b), kHead, who);
        return b;
    }
}

// The ring has no room for the banner: the line is refused, its need is its own length, and
// neither it nor a queued byte goes out through the polled writer.
TEST(ConsoleRecordRoom, ARecordLineBehindAFullRingIsRefusedAndSendsNothing)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        consoleseam::set_isr_runs_in_gap(false);
        std::string const queued = fill();
        ASSERT_FALSE(queued.empty());
        ASSERT_LT(kRing - 1u - queued.size(), head("t1").size())
            << "premise: the ring has no room for the record";
        std::string const before = consoleseam::wire();

        uint32_t const need = kickos::kprintf_fault(kHead, "t1");
        EXPECT_EQ(need, static_cast<uint32_t>(head("t1").size()));
        EXPECT_EQ(consoleseam::sync_calls(), 0u) << "the refused record line used the polled writer";
        EXPECT_EQ(consoleseam::wire(), before) << "the refused record line sent queued bytes";

        console_tx_flush_sync();
        EXPECT_EQ(consoleseam::wire(), queued) << "the refused line reached the ring";
    });
}

// Offered again once the drain made room, the line queues behind the lines ahead of it, whole.
TEST(ConsoleRecordRoom, ARefusedRecordLineOfferedAgainFollowsTheQueuedLines)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        consoleseam::set_isr_runs_in_gap(false);
        std::string const queued = fill();
        ASSERT_NE(kickos::kprintf_fault(kHead, "t1"), 0u);
        console_tx_isr();
        EXPECT_EQ(kickos::kprintf_fault(kHead, "t1"), 0u);
        console_tx_flush_sync();
        EXPECT_EQ(consoleseam::wire(), queued + head("t1"));
        EXPECT_EQ(consoleseam::sync_calls(), 0u);
    });
}

// A line with room goes at once, whatever is queued.
TEST(ConsoleRecordRoom, ARecordLineWithRoomQueuesAtOnce)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        consoleseam::set_isr_runs_in_gap(false);
        EXPECT_EQ(kickos::kprintf_fault(kPc, 0x100u), 0u);
        console_tx_flush_sync();
        EXPECT_EQ(consoleseam::wire(), "  PC=0x100\n");
    });
}

// The room a refusal waits for counts the CR each '\n' gains on a CRLF console.
TEST(ConsoleRecordRoom, TheNeedCountsTheCarriageReturns)
{
    EXPECT_EQ(console_tx_line_needed("ab\ncd\n", 6, 1), 8u);
    EXPECT_EQ(console_tx_line_needed("ab\ncd\n", 6, 0), 6u);
}
