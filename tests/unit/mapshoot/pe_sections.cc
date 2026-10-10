// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The x86_64 user-pointer oracle's section rules, over a section table built here.

#include <kickos/arch/pe_sections.h>

#include <gtest/gtest.h>

#include <string.h>

using kickos::x86_64::pe_range_mapped;

namespace
{
    constexpr uint32_t SCN_EXECUTE = 0x20000000u;
    constexpr uint32_t SCN_READ = 0x40000000u;
    constexpr uint32_t SCN_WRITE = 0x80000000u;
    constexpr uint32_t SCN_DISCARDABLE = 0x02000000u;

    constexpr uintptr_t BASE = 0x400000u;
    constexpr size_t SIZE = 0x6000u;

    // .text at 0x1000, .rdata at 0x2000, a discardable .reloc at 0x3000, and .data at 0x4000,
    // whose raw size outgrows both its virtual size and the image.
    constexpr uintptr_t TEXT = BASE + 0x1000u;
    constexpr uintptr_t RDATA = BASE + 0x2000u;
    constexpr uintptr_t RELOC = BASE + 0x3000u;
    constexpr uintptr_t DATA = BASE + 0x4000u;

    struct Image
    {
        alignas(4) uint8_t headers[4 * 40] = {};

        void section(unsigned i, uint32_t rva, uint32_t virtual_size, uint32_t raw_size,
                     uint32_t characteristics)
        {
            uint8_t* const s = headers + i * 40u;
            memcpy(s + 8, &virtual_size, 4);
            memcpy(s + 12, &rva, 4);
            memcpy(s + 16, &raw_size, 4);
            memcpy(s + 36, &characteristics, 4);
        }

        Image()
        {
            section(0, 0x1000u, 0x0800u, 0x1000u, SCN_EXECUTE | SCN_READ);
            section(1, 0x2000u, 0x1000u, 0x0200u, SCN_READ);
            section(2, 0x3000u, 0x1000u, 0x1000u, SCN_READ | SCN_DISCARDABLE);
            section(3, 0x4000u, 0x0100u, 0x3000u, SCN_READ | SCN_WRITE);
        }

        bool reads(uintptr_t ptr, size_t len) const
        {
            return pe_range_mapped(BASE, SIZE, headers, 4, ptr, len, false);
        }

        bool writes(uintptr_t ptr, size_t len) const
        {
            return pe_range_mapped(BASE, SIZE, headers, 4, ptr, len, true);
        }
    };
}

TEST(PeSections, ReadAdmitsTextReadOnlyDataAndData)
{
    Image const img;
    EXPECT_TRUE(img.reads(TEXT, 8));
    EXPECT_TRUE(img.reads(RDATA, 8));
    EXPECT_TRUE(img.reads(DATA, 8));
}

TEST(PeSections, ReadAdmitsARangeAsLongAsTheLargerOfItsTwoSizesWithinTheImage)
{
    Image const img;
    EXPECT_TRUE(img.reads(TEXT, 0x1000u));
    EXPECT_TRUE(img.reads(DATA, 0x2000u));
    EXPECT_FALSE(img.reads(DATA, 0x2001u));
}

TEST(PeSections, ReadRefusesOutsideTheImageAndAcrossTwoSections)
{
    Image const img;
    EXPECT_FALSE(img.reads(0, 8));
    EXPECT_FALSE(img.reads(BASE - 8u, 8));
    EXPECT_FALSE(img.reads(BASE + SIZE, 8));
    EXPECT_FALSE(img.reads(RDATA - 4u, 8));
    EXPECT_FALSE(img.reads(BASE, 8));
}

TEST(PeSections, ReadRefusesADiscardableSection)
{
    Image const img;
    EXPECT_FALSE(img.reads(RELOC, 8));
}

TEST(PeSections, ReadRefusesAWrappingRange)
{
    Image const img;
    EXPECT_FALSE(img.reads(TEXT, SIZE_MAX));
}

TEST(PeSections, WriteAdmitsDataAndRefusesTextAndReadOnlyData)
{
    Image const img;
    EXPECT_TRUE(img.writes(DATA, 8));
    EXPECT_FALSE(img.writes(TEXT, 8));
    EXPECT_FALSE(img.writes(RDATA, 8));
}

TEST(PeSections, AnEmptyRangeIsAdmittedAndNoTableAdmitsNothing)
{
    Image const img;
    EXPECT_TRUE(img.reads(0, 0));
    EXPECT_TRUE(img.writes(TEXT, 0));
    EXPECT_FALSE(pe_range_mapped(BASE, SIZE, nullptr, 0, DATA, 8, false));
}
