// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// console_tx_insert_line: the unit of atomicity is a LINE, so a line the ring cannot hold is
// refused whole and the refusal empties the ring first, and '\n' is lowered to CR+LF inside
// the copy with the space check counting the expansion.
//
// Each arm names its own geometry and its own arming, and every drain is delivered by the arm
// itself, so the same source runs the same way whatever the host is doing.

#include <gtest/gtest.h>

#include <string>

#include <kickos/console_tx.h>

#include "line_seam.h"

namespace
{
    // Usable capacity is size-1, and the geometry below is picked so one 8-byte line plus its
    // prime leaves exactly 8 bytes of space.
    constexpr uint32_t kRing = 16u;
    constexpr int kIrqLine = 3;
    constexpr int kNoIrqLine = -1;

    class ConsoleTxLine : public ::testing::Test
    {
    protected:
        void SetUp() override { consoleline::reset(kRing, kNoIrqLine); }
    };
}

// A caller that hands over nothing owes no fallback.
TEST_F(ConsoleTxLine, AnEmptyLineIsTakenAndOwesNoFallback)
{
    EXPECT_EQ(consoleline::write_line("", 0, 0), 1);
    EXPECT_TRUE(consoleline::wire().empty());
}

// Enabling the TX interrupt on an idle channel raises nothing where the event is transition
// triggered, so the insert pushes the first byte itself and that push is what starts the drain.
TEST_F(ConsoleTxLine, ALineIntoAnIdleChannelPutsItsFirstByteOnTheWire)
{
    consoleline::reset(kRing, kIrqLine);
    ASSERT_EQ(consoleline::write_line("hello", 5, 0), 1);
    EXPECT_EQ(consoleline::wire(), std::string("h"));

    consoleline::pump_tx_isr();
    EXPECT_EQ(consoleline::wire(), std::string("hello"));
    EXPECT_GT(consoleline::isr_entries(), 0u) << "the drain never ran, so the bytes after the "
                                                 "prime were never the ISR's to carry";
}

// A backend with no TX interrupt is drained in the producer's own context, and the whole line
// is on the wire by the time the insert returns.
TEST_F(ConsoleTxLine, ALineAtAChannelWithNoTxInterruptIsDrainedBeforeTheInsertReturns)
{
    ASSERT_EQ(consoleline::write_line("hello", 5, 0), 1);
    EXPECT_EQ(consoleline::wire(), std::string("hello"));

    consoleline::pump_tx_isr();
    EXPECT_EQ(consoleline::wire(), std::string("hello")) << "the ring still held bytes, so the "
                                                            "producer drain left work behind";
}

// A line the ring cannot take does not reach the device at all, and the lines already queued
// are untouched by the refusal. The defect this pins is a SPLIT line: a fallback writing
// straight at the device while the drain pushes ring bytes puts two writers on one wire, and
// the refused line lands inside an earlier one.
TEST_F(ConsoleTxLine, ARefusedLineReachesTheDeviceNotAtAll)
{
    consoleline::reset(kRing, kIrqLine);
    std::string const first(8, 'A');
    std::string const second(8, 'B');
    std::string const third(4, 'C');

    ASSERT_EQ(consoleline::write_line(first.data(), first.size(), 0), 1);
    ASSERT_EQ(consoleline::wire().size(), 1u) << "the prime alone; the rest has to be queued "
                                                 "for this arm to read an overtake at all";
    ASSERT_EQ(consoleline::write_line(second.data(), second.size(), 0), 1);
    ASSERT_EQ(consoleline::wire().size(), 1u);

    EXPECT_EQ(consoleline::write_line(third.data(), third.size(), 0), 0)
        << "the third line fitted, so nothing here exercises a refusal";
    // Drains what the ring holds, so a refused line that had reached it anyway would show up
    // here rather than hide behind an empty wire.
    console_tx_flush_sync();
    EXPECT_EQ(consoleline::wire(), first + second)
        << "the refused line reached the device, so a second writer exists";
}

// CR+LF is built during the copy, in the ring.
TEST_F(ConsoleTxLine, ANewlineIsLoweredToCarriageReturnLineFeedInTheRing)
{
    ASSERT_EQ(consoleline::write_line("a\nb", 3, 1), 1);
    EXPECT_EQ(consoleline::wire(), std::string("a\r\nb"));
}

// The space check counts the expansion, so a line of four newlines needs eight bytes. Eight is
// exactly what is free here, and the line is taken.
TEST_F(ConsoleTxLine, ALineWhoseExpansionExactlyFillsTheRingIsTaken)
{
    consoleline::reset(kRing, kIrqLine);
    std::string const prefill(8, 'P');
    ASSERT_EQ(consoleline::write_line(prefill.data(), prefill.size(), 0), 1);
    ASSERT_EQ(consoleline::wire().size(), 1u);

    std::string const line(4, '\n');
    EXPECT_EQ(consoleline::write_line(line.data(), line.size(), 1), 1);
    console_tx_flush_sync();
    EXPECT_EQ(consoleline::wire(), prefill + "\r\n\r\n\r\n\r\n");
}

// One byte more of expansion than the ring has free, with the raw length still fitting: the
// refusal has to be settled on the expanded size.
TEST_F(ConsoleTxLine, ALineWhoseExpansionOverrunsTheRingByOneIsRefused)
{
    consoleline::reset(kRing, kIrqLine);
    std::string const prefill(8, 'P');
    ASSERT_EQ(consoleline::write_line(prefill.data(), prefill.size(), 0), 1);
    ASSERT_EQ(consoleline::wire().size(), 1u);

    std::string line(4, '\n');
    line.push_back('X');
    EXPECT_EQ(consoleline::write_line(line.data(), line.size(), 1), 0);
    console_tx_flush_sync();
    EXPECT_EQ(consoleline::wire(), prefill)
        << "the refused line reached the device, so the expanded size did not settle it";
}

// A synchronous CPU fault is not gated by the interrupt mask, so its reporter's line can enter
// the insert with the copy's head unpublished. The nested line is refused and does not go out,
// which is what keeps the outer publication from swallowing it.
namespace
{
    int g_nested_took = -1;

    void insert_from_inside_the_copy(void)
    {
        g_nested_took = consoleline::write_line("NEST", 4, 0);
    }
}

TEST_F(ConsoleTxLine, ALineInsertedInsideAnotherCopyIsRefused)
{
    g_nested_took = -1;
    consoleline::run_at_publish_barrier(insert_from_inside_the_copy);

    ASSERT_EQ(consoleline::write_line("OUTER", 5, 0), 1);
    ASSERT_TRUE(consoleline::barrier_seat_fired()) << "the seated line never entered the "
                                                      "insert, so no re-entry was attempted";
    EXPECT_EQ(g_nested_took, 0);
    EXPECT_EQ(consoleline::wire(), std::string("OUTER"))
        << "the nested line reached the device, so the outer copy was published over it";
}

// A disarmed ring is the ONE case that still writes straight at the device: no ring means no
// drain, so no second writer exists to interleave with. The insert owns that decision under
// the same lock as the rest, so it cannot race an arm, and it reports the line as taken.
TEST_F(ConsoleTxLine, ADisarmedRingWritesStraightAtTheDevice)
{
    console_tx_deinit();
    ASSERT_EQ(console_tx_armed(), 0);

    EXPECT_EQ(consoleline::write_line("hello", 5, 0), 1);
    EXPECT_EQ(consoleline::wire(), std::string("hello"));
}
