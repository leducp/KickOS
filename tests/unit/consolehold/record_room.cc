// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A thread-fault record on the kernel's own console reaches the wire whole when the ring is
// full: each of its lines makes room by sending the oldest queued bytes under the mask, never
// more than the line's own length, and the queued lines go out first and unsplit, once each.

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

    // Ordinary lines leaving at most `room` bytes free, queued by the burst writer, which never
    // drains in its producer: what a backend with no TX interrupt holds while none is draining.
    std::string fill_undrained(uint32_t room)
    {
        std::string queued;
        for (char c = 'a'; queued.size() + 41u + room <= kRing - 1u; c++)
        {
            queued += std::string(40, c) + "\n";
        }
        console_tx_write(queued.data(), queued.size());
        return queued;
    }

    int head_len(char const* who)
    {
        char head[96];
        return snprintf(head, sizeof(head), kHead, who);
    }

    std::string g_seen;
    int g_nested = -1;
    std::string const kNested = "nested\n";
    std::string const kLine = "  record line\n";
    std::string const kWide(kRing - 1u, 'x');
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

// A backend with no TX interrupt drains in its producer, and a record line runs that drain only
// once the mask is dropped.
TEST(ConsoleRecordRoom, NoTxInterruptDrainsTheRestUnmasked)
{
    run_isolated([]() {
        consoleseam::reset(kRing, -1);
        std::string const queued = fill_undrained(0u);
        ASSERT_LT(kRing - 1u - (queued.size() - consoleseam::wire().size()),
                  static_cast<uint32_t>(head_len("t1")))
            << "premise: the ring has no room for the record's first line";

        record("t1", 0x100u);
        EXPECT_LE(consoleseam::max_masked_pushes(), static_cast<uint32_t>(head_len("t1")))
            << "a record line sent more than its own length under the mask";
        console_tx_flush_sync();
        EXPECT_EQ(consoleseam::wire(), queued + text("t1", 0x100u));
    });
}

// The drain ISR running in every gap the record leaves takes the bytes the record did not.
TEST(ConsoleRecordRoom, TheDrainIsrInTheGapsKeepsTheOrder)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        consoleseam::set_isr_runs_in_gap(false);
        std::string const queued = fill();
        consoleseam::set_isr_runs_in_gap(true);
        consoleseam::set_gap_budget(4u);

        record("t1", 0x100u);
        EXPECT_LE(consoleseam::max_masked_pushes(), static_cast<uint32_t>(head_len("t1")));
        console_tx_flush_sync();
        EXPECT_EQ(consoleseam::wire(), queued + text("t1", 0x100u));
    });
}

// The room counts the CR each '\n' gains.
TEST(ConsoleRecordRoom, ACrlfLineMakesRoomForItsCarriageReturns)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        consoleseam::set_isr_runs_in_gap(false);
        std::string const queued = fill();
        std::string const line = "ab\ncd\nef\n";
        uint32_t const expanded = static_cast<uint32_t>(line.size()) + 3u;
        ASSERT_LT(kRing - 1u - queued.size(), static_cast<size_t>(expanded));

        EXPECT_EQ(console_tx_insert_record_line(line.data(), line.size(), 1),
                  static_cast<int>(line.size()));
        EXPECT_LE(consoleseam::max_masked_pushes(), expanded);
        console_tx_flush_sync();
        EXPECT_EQ(consoleseam::wire(), queued + "ab\r\ncd\r\nef\r\n");
    });
}

// A line as wide as the ring's whole room sends every queued byte first.
TEST(ConsoleRecordRoom, ALineOfTheWholeRoomFollowsTheWholeRing)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        consoleseam::set_isr_runs_in_gap(false);
        std::string const queued = fill();
        std::string const line(kRing - 1u, 'z');
        EXPECT_EQ(console_tx_insert_record_line(line.data(), line.size(), 0),
                  static_cast<int>(line.size()));
        console_tx_flush_sync();
        EXPECT_EQ(consoleseam::wire(), queued + line);
    });
}

// A producer drain holds no byte between its pushes: a record line arriving there makes its room
// in ring order, after the line the drain is sending.
TEST(ConsoleRecordRoom, ARecordBetweenAProducerDrainsPushesTakesItsRoomInOrder)
{
    run_isolated([]() {
        consoleseam::reset(kRing, -1);
        std::string const queued = fill_undrained(64u);
        // The insert's gap, then the one after the drain claims the ring.
        consoleseam::run_in_gap(consoleseam::gap_count() + 2u, []() {
            g_nested = console_tx_insert_record_line(kWide.data(), kWide.size(), 0);
        });
        EXPECT_EQ(console_tx_insert_line(kLine.data(), kLine.size(), 0),
                  static_cast<int>(kLine.size()));
        ASSERT_TRUE(consoleseam::seat_fired());
        EXPECT_EQ(g_nested, static_cast<int>(kWide.size()));
        EXPECT_EQ(consoleseam::wire(), queued + kLine + kWide);
    });
}

// A fault in the polled writer whose reporter offers a record line: refused, and the outer line
// still follows every queued byte once.
TEST(ConsoleRecordRoom, ARecordNestedInTheRoomIsRefused)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        consoleseam::set_isr_runs_in_gap(false);
        std::string const queued = fill();
        consoleseam::run_in_sync_write(1u, []() {
            g_nested = console_tx_insert_record_line(kNested.data(), kNested.size(), 0);
        });
        std::string const line(64u, 'r');
        EXPECT_EQ(console_tx_insert_record_line(line.data(), line.size(), 0),
                  static_cast<int>(line.size()));
        EXPECT_EQ(g_nested, 0);
        console_tx_flush_sync();
        EXPECT_EQ(consoleseam::wire(), queued + line);
    });
}

// A panic flush taken in the polled writer sends each queued byte once: the rest of the run in
// flight is not the flush's to send, and the interrupted record line moves no tail back when it
// resumes.
TEST(ConsoleRecordRoom, APanicFlushInTheRoomResendsNothing)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        consoleseam::set_isr_runs_in_gap(false);
        std::string const queued = fill();
        std::string const line(64u, 'r');
        size_t const primed = consoleseam::wire().size();
        size_t const run = line.size() - (kRing - 1u - (queued.size() - primed));
        consoleseam::run_in_sync_write(1u, []() {
            console_tx_flush_sync();
            g_seen = consoleseam::wire();
        });
        EXPECT_EQ(console_tx_insert_record_line(line.data(), line.size(), 0),
                  static_cast<int>(line.size()));
        EXPECT_EQ(g_seen, queued.substr(0, primed + 1u) + queued.substr(primed + run));
        console_tx_flush_sync();
        EXPECT_EQ(consoleseam::wire(), g_seen + queued.substr(primed + 1u, run - 1u) + line);
    });
}

// A wedged channel: every record line still queues, and each costs at most one stall window of
// the polled writer under the mask, even where the room it makes wraps the ring's end.
TEST(ConsoleRecordRoom, AWedgedChannelCostsOneStallPerLine)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        consoleseam::set_isr_runs_in_gap(false);
        std::string const lead(kRing - 6u, 'y');
        ASSERT_NE(console_tx_insert_line(lead.data(), lead.size(), 0), 0);
        console_tx_flush_sync();
        std::string const queued = fill();
        ASSERT_LT(kRing - 1u - queued.size(), static_cast<size_t>(head_len("t1")))
            << "premise: the record's first line needs room past the ring's end";
        ASSERT_GT(static_cast<size_t>(head_len("t1")) - (kRing - 1u - queued.size()), 6u)
            << "premise: the room the first line makes wraps the ring's end";

        consoleseam::set_sync_stalls(true);
        record("t1", 0x100u);
        EXPECT_GE(consoleseam::max_masked_stalls(), 1u) << "premise: no line met the stall";
        EXPECT_LE(consoleseam::max_masked_stalls(), 1u)
            << "a record line spent more than one stall window under the mask";
        consoleseam::set_sync_stalls(false);
        console_tx_flush_sync();
        std::string const sent = consoleseam::wire();
        std::string const want = text("t1", 0x100u);
        ASSERT_GE(sent.size(), want.size());
        EXPECT_EQ(sent.substr(sent.size() - want.size()), want);
    });
}
