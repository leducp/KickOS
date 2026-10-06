// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A thread-fault record on the kernel's own console reaches the wire whole when the ring is
// full: each of its lines makes room by sending the oldest queued bytes under the mask, never
// more than the line's own length, and the queued lines go out first and unsplit.

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
}

TEST(ConsoleRecordRoom, ARecordReachesTheWireWholeBehindAFullRing)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        consoleseam::set_isr_runs_in_gap(false);
        std::string const queued = fill();
        ASSERT_FALSE(queued.empty());
        ASSERT_LT(kRing - 1u - queued.size(), text("t1", 0x100u).size())
            << "premise: the ring has no room for the record";

        record("t1", 0x100u);
        std::string const pushed = consoleseam::wire();
        EXPECT_EQ(pushed, queued.substr(0, pushed.size()))
            << "the room was made out of order, or out of another line";
        char head[96];
        int const head_len = snprintf(head, sizeof(head), kHead, "t1");
        EXPECT_LE(consoleseam::max_masked_pushes(), static_cast<uint32_t>(head_len))
            << "a record line masked more than its own length of wire time";

        console_tx_flush_sync();
        EXPECT_EQ(consoleseam::wire(), queued + text("t1", 0x100u));
    });
}

// A line wider than the ring can never fit, and is refused before anything is sent for it.
TEST(ConsoleRecordRoom, ALineWiderThanTheRingIsRefusedAndSendsNothing)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        consoleseam::set_isr_runs_in_gap(false);
        ASSERT_FALSE(fill().empty());
        std::string const before = consoleseam::wire();
        std::string const wide(kRing, 'x');
        EXPECT_EQ(console_tx_insert_record_line(wide.data(), wide.size(), 0), 0);
        EXPECT_EQ(consoleseam::wire(), before);
    });
}
