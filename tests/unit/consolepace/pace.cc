// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Test a burst larger than the console ring against a rate-limited transport.
// The printer must retry rejected lines. consoleseam limits bytes per window;
// emulated UARTs drain immediately and do not exercise this case.

#include <gtest/gtest.h>

#include <string>

#include <kickos/console_tx.h>
#include <kickos/kernel.h>

#include "console_seam.h"

namespace
{
    // The rows the bench phase table prints, at its width and its count.
    constexpr uint32_t kRows = 42u;
    constexpr uint32_t kGapBudget = 4u;

    void arm(uint32_t gap_budget)
    {
        consoleseam::reset(KICKOS_CONSOLE_TX_SIZE);
        consoleseam::set_gap_budget(gap_budget);
    }

    // Rows that reached the wire whole, and the first one that did not. Not every row after
    // the first loss is lost: the drain keeps running, so a later line can fit where its
    // predecessor did not. What the refusal guarantees is that rows go missing WHOLE.
    uint32_t landed(uint32_t* first_missing)
    {
        std::string const& wire = consoleseam::wire();
        uint32_t seen = 0;
        *first_missing = kRows;
        size_t at = 0;
        for (uint32_t i = 0; i < kRows; i++)
        {
            char want[16];
            snprintf(want, sizeof(want), "ROW%02u ", static_cast<unsigned>(i));
            size_t const hit = wire.find(want, at);
            if (hit == std::string::npos)
            {
                if (*first_missing == kRows)
                {
                    *first_missing = i;
                }
                continue;
            }
            at = hit;
            seen++;
        }
        return seen;
    }

    // The burst, then the flush an image takes on its way out. WITHOUT THE FLUSH the last
    // ring-full has been accepted and not yet pushed, and a row still queued would be counted
    // as a row lost.
    void print_table(bool paced)
    {
        for (uint32_t i = 0; i < kRows; i++)
        {
            if (paced)
            {
                kickos::kprintf_paced("    ROW%02u %u/%u  min=%u  n=%u\n",
                                      static_cast<unsigned>(i), 3418u, 3883u, 3238u, 200000u);
            }
            else
            {
                kickos::kprintf("    ROW%02u %u/%u  min=%u  n=%u\n", static_cast<unsigned>(i),
                                3418u, 3883u, 3238u, 200000u);
            }
        }
        console_tx_flush_sync();
    }
}

// THE PLANTED VIOLATION: the first rows arrive, the ring fills, and every row after it is
// refused whole.
TEST(ConsolePace, UnpacedPrinterLosesTheTailOfTheBurst)
{
    arm(kGapBudget);
    print_table(false);
    uint32_t first_missing = 0;
    uint32_t const seen = landed(&first_missing);
    EXPECT_LT(seen, kRows);
    // The ring holds the head of the burst: a mock that dropped from the first row would be
    // measuring a refusal that has nothing to do with filling it.
    EXPECT_GT(first_missing, 0u);
}

TEST(ConsolePace, PacedPrinterLandsEveryRow)
{
    arm(kGapBudget);
    print_table(true);
    uint32_t first_missing = 0;
    EXPECT_EQ(landed(&first_missing), kRows);
}

// A WEDGED CHANNEL MUST NOT HANG THE RUN. Nothing drains, so the paced printer loses the
// lines a plain one would and returns, which is what keeps a bench bounded on a board that
// gets itself back through KICKOS_SHUTDOWN_TO_BOOTLOADER.
TEST(ConsolePace, PacedPrinterGivesUpOnAChannelThatNeverDrains)
{
    arm(kGapBudget);
    consoleseam::set_isr_runs_in_gap(false);
    print_table(true);
    uint32_t first_missing = 0;
    uint32_t const seen = landed(&first_missing);
    EXPECT_LT(seen, kRows);
    EXPECT_GT(consoleseam::wire().size(), 0u);
}
