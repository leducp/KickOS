// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <gtest/gtest.h>

#include <string>

// The host sim deliberately leaves its TAP stream raw. Exercise the AMP UART's CRLF arm.
#undef KICKOS_CONSOLE_CRLF
#define KICKOS_CONSOLE_CRLF 1
#include <kickos/arch/console_retry.h>

#include "line_seam.h"

TEST(ConsoleRetry, AClaimLostBetweenCrAndLfResumesWithoutRepeatingCr)
{
    std::string wire;
    bool pending = false;
    int attempt = 0;
    auto put = [&](char c) {
        attempt++;
        if (attempt == 3) // a, CR landed; LF loses the claim
        {
            return false;
        }
        wire.push_back(c);
        return true;
    };
    char const line[] = "a\nb";
    EXPECT_EQ(kickos::console_retry::write(line, 3, &pending, put), 1u);
    EXPECT_TRUE(pending);
    EXPECT_EQ(wire, "a\r");

    EXPECT_EQ(kickos::console_retry::write(line + 1, 2, &pending, put), 2u);
    EXPECT_FALSE(pending);
    EXPECT_EQ(wire, "a\r\nb");
}

TEST(ConsoleRetry, RepeatedRefusalsKeepThePendingCrWithItsWriter)
{
    std::string wire;
    bool first_pending = false;
    bool second_pending = false;
    auto store = [&](char c) {
        wire.push_back(c);
        return true;
    };
    auto refuse_lf = [&](char c) {
        if (c == '\n')
        {
            return false;
        }
        return store(c);
    };

    EXPECT_EQ(kickos::console_retry::write("\n", 1, &first_pending, refuse_lf), 0u);
    EXPECT_TRUE(first_pending);
    EXPECT_EQ(kickos::console_retry::write("x\n", 2, &second_pending, store), 2u);
    EXPECT_EQ(kickos::console_retry::write("\n", 1, &first_pending, refuse_lf), 0u);
    EXPECT_TRUE(first_pending);
    EXPECT_EQ(kickos::console_retry::write("\n", 1, &first_pending, store), 1u);
    EXPECT_FALSE(first_pending);
    EXPECT_EQ(wire, "\rx\r\n\n");
}

// Each wire byte is its own masked span, CR included: interrupts are open between two bytes.
TEST(ConsoleRetry, AMaskedWriteMasksOneByteAtATime)
{
    std::string wire;
    std::string depths;
    std::string spans;
    bool pending = false;
    uint32_t const before = consoleline::mask_spans();
    auto put = [&](char c) {
        wire.push_back(c);
        depths.push_back(static_cast<char>('0' + consoleline::mask_depth()));
        spans.push_back(static_cast<char>('0' + (consoleline::mask_spans() - before)));
        return true;
    };
    ASSERT_EQ(consoleline::mask_depth(), 0);
    EXPECT_EQ(kickos::console_retry::write_masked("ab\nc", 4, &pending, put), 4u);
    EXPECT_EQ(wire, "ab\r\nc");
    EXPECT_EQ(depths, "11111") << "a byte went out unmasked";
    EXPECT_EQ(spans, "12345") << "a masked span carried more than one byte";
    EXPECT_EQ(consoleline::mask_depth(), 0);
}
