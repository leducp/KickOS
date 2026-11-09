// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The ESP32 cache MMU entries of VAddr1 for PID 0 and 1 against ESP32 TRM v5.8 Tables 4.3-9 to
// 4.3-11 (p.83-84), and the flash page an entry names (4.3.2.2, p.84).

#include "arch/xtensa/chip/esp32/flash_mmu.h"

#include <gtest/gtest.h>

#include <stdint.h>

#include <utility>
#include <vector>

namespace m = kickos::esp32::flash_mmu;

TEST(FlashMmu, vaddr1_entry_per_core)
{
    struct Case
    {
        uint32_t core;
        uint32_t va;
        uint32_t entry;
        char const* what;
    };
    Case const cases[] = {
        {0u, 0x40000000u, 64u, "PRO_CPU's VAddr1 page 0, Table 4.3-10"},
        {1u, 0x40000000u, 2112u, "APP_CPU's VAddr1 page 0, Table 4.3-11"},
        {0u, 0x400D0000u, 77u, "PRO_CPU's first page clear of on-chip memory"},
        {1u, 0x400D0000u, 2125u, "APP_CPU's first page clear of on-chip memory"},
        {0u, 0x400DFFFFu, 77u, "the last byte of that page"},
        {0u, 0x400E0000u, 78u, "the next page"},
        {1u, 0x403F0000u, 2175u, "APP_CPU's last VAddr1 page"},
        {0u, 0x40400000u, m::ENTRIES, "VAddr2 is no VAddr1 page"},
        {0u, 0x3FFFFFFCu, m::ENTRIES, "below VAddr1"},
        {2u, 0x400D0000u, m::ENTRIES, "no third core"},
    };
    for (Case const& c : cases)
    {
        EXPECT_EQ(m::vaddr1_entry(c.core, c.va), c.entry) << c.what;
    }
}

TEST(FlashMmu, entry_value_names_the_flash_page)
{
    EXPECT_EQ(m::entry_value(0x70000u), 7u);
    EXPECT_EQ(m::entry_value(0x7FFFCu), 7u);
    EXPECT_EQ(m::entry_value(0xFF0000u), 255u);
    EXPECT_EQ(m::entry_value(0x1000000u), m::INVALID) << "16 MiB is past the eight address bits";
    EXPECT_EQ(m::INVALID, 0x100u);
}

TEST(FlashMmu, pages_of_a_span)
{
    EXPECT_EQ(m::pages(0x400D0000u, 0x400F0000u), 2u);
    EXPECT_EQ(m::pages(0x400D0000u, 0x400D0000u), 0u);
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

TEST(FlashMmu, map_text_writes_a_page_per_core)
{
    Writes w;
    EXPECT_TRUE(m::map_text(2u, 0x400D0000u, 0x400F0000u, 0x70000u, w));
    ASSERT_EQ(w.rows.size(), 4u);
    EXPECT_EQ(w.rows[0], std::make_pair(77u, 7u));
    EXPECT_EQ(w.rows[1], std::make_pair(2125u, 7u));
    EXPECT_EQ(w.rows[2], std::make_pair(78u, 8u));
    EXPECT_EQ(w.rows[3], std::make_pair(2126u, 8u));
}

TEST(FlashMmu, map_text_ends_on_the_last_vaddr1_page)
{
    Writes w;
    EXPECT_TRUE(m::map_text(1u, 0x40300000u, 0x40400000u, 0x70000u, w));
    EXPECT_EQ(w.rows.size(), 16u);
    EXPECT_EQ(w.rows.back().first, 127u);
}

TEST(FlashMmu, map_text_refuses_a_page_past_vaddr1_and_writes_nothing)
{
    Writes w;
    EXPECT_FALSE(m::map_text(2u, 0x403F0000u, 0x40410000u, 0x70000u, w));
    EXPECT_TRUE(w.rows.empty()) << "a page before the bad one was written";
}
