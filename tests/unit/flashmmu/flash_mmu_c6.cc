// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The ESP32-C6 cache MMU entries over the external memory window 0x4200_0000 to 0x42FF_FFFF
// (TRM v1.2 5.3.3.1), 64 KiB pages, and the flash page an entry names.

#include "arch/riscv/chip/esp32c6/flash_mmu.h"

#include <gtest/gtest.h>

#include <stdint.h>

#include <utility>
#include <vector>

namespace m = kickos::esp32c6::flash_mmu;

TEST(FlashMmuC6, entry_of_the_window)
{
    struct Case
    {
        uint32_t va;
        uint32_t entry;
        char const* what;
    };
    Case const cases[] = {
        {0x42000000u, 0u, "the window's first page"},
        {0x4200FFFFu, 0u, "the last byte of that page"},
        {0x42010000u, 1u, "the next page"},
        {0x42FF0000u, 255u, "the window's last page"},
        {0x43000000u, m::ENTRIES, "past the window"},
        {0x41FFFFFCu, m::ENTRIES, "below the window"},
        {0x40800000u, m::ENTRIES, "HP SRAM is no window page"},
    };
    for (Case const& c : cases)
    {
        EXPECT_EQ(m::entry_of(c.va), c.entry) << c.what;
    }
}

TEST(FlashMmuC6, entry_value_names_the_flash_page)
{
    EXPECT_EQ(m::entry_value(0x90000u), m::VALID | 9u);
    EXPECT_EQ(m::entry_value(0x9FFFCu), m::VALID | 9u);
    EXPECT_EQ(m::entry_value(0xFF0000u), m::VALID | 255u);
    EXPECT_EQ(m::entry_value(0x1000000u), m::INVALID) << "16 MiB is past what an entry names";
    EXPECT_EQ(m::VALID, 0x200u);
    EXPECT_EQ(m::INVALID, 0u);
}

namespace
{
    struct Writes
    {
        std::vector<std::pair<uint32_t, uint32_t>> rows;

        void operator()(uint32_t entry, uint32_t value)
        {
            rows.emplace_back(entry, value);
        }
    };
}

TEST(FlashMmuC6, map_text_writes_one_entry_a_page)
{
    Writes w;
    EXPECT_TRUE(m::map_text(0x42000000u, 0x42020000u, 0x90000u, w));
    ASSERT_EQ(w.rows.size(), 2u);
    EXPECT_EQ(w.rows[0], std::make_pair(0u, m::VALID | 9u));
    EXPECT_EQ(w.rows[1], std::make_pair(1u, m::VALID | 10u));
}

TEST(FlashMmuC6, map_text_ends_on_the_last_window_page)
{
    Writes w;
    EXPECT_TRUE(m::map_text(0x42F00000u, 0x43000000u, 0x0u, w));
    EXPECT_EQ(w.rows.size(), 16u);
    EXPECT_EQ(w.rows.back().first, 255u);
}

TEST(FlashMmuC6, map_text_refuses_a_page_past_the_window_and_writes_nothing)
{
    Writes w;
    EXPECT_FALSE(m::map_text(0x42FF0000u, 0x43010000u, 0x90000u, w));
    EXPECT_TRUE(w.rows.empty()) << "a page before the bad one was written";
}

TEST(FlashMmuC6, map_text_refuses_flash_no_entry_names_and_writes_nothing)
{
    Writes w;
    EXPECT_FALSE(m::map_text(0x42000000u, 0x42020000u, 0xFF0000u, w));
    EXPECT_TRUE(w.rows.empty()) << "the page an entry names was written";
}
