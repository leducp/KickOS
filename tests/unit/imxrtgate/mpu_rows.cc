// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The i.MX RT1062 fixed MPU rows, resolved address by address the way PMSAv7 does: the highest
// enabled region whose enabled subregion holds the address decides, and an address no row holds
// falls to the PRIVDEFENA default map (DDI0403E B3.5.1, B3.5.8). The memory map is
// IMXRT1060RM Table 3-1; the Teensy 4.1 populates 8 MiB of FlexSPI flash.

#include "arch/arm/chip/imxrt1062/mpu_rows.h"

#include <kickos/arch/mpu_encoded.h>

#include <gtest/gtest.h>

#include <stdint.h>

namespace
{
    namespace mr = kickos::imxrt1062::mpu_rows;

    constexpr uint32_t FLASH_BASE = 0x60000000u;
    constexpr uint32_t FLASH_SIZE = 0x800000u;
    constexpr uint32_t OCRAM2_BASE = 0x20200000u;
    constexpr uint32_t OCRAM2_SIZE = 0x80000u;
    // IMXRT1060RM 28.1.
    constexpr uint32_t HW_REGIONS = 16u;

    constexpr mr::Rows TABLE = mr::rows(FLASH_BASE, FLASH_SIZE, OCRAM2_BASE, OCRAM2_SIZE);

    enum class Kind
    {
        unmatched,
        strongly_ordered,
        device,
        normal,
    };

    struct Hit
    {
        Kind kind = Kind::unmatched;
        uint32_t ap = 0;
        bool xn = false;
        uint32_t tex_c_b = 0;
    };

    Hit resolve(uint32_t addr)
    {
        for (size_t i = mr::COUNT; i > 0; i--)
        {
            uint32_t const rasr = TABLE.row[i - 1].rasr;
            if ((rasr & 1u) == 0u)
            {
                continue;
            }
            uint64_t const size = uint64_t{1} << (((rasr >> 1) & 0x1Fu) + 1u);
            uint64_t const base = TABLE.row[i - 1].base & ~(size - 1u);
            if (addr < base or addr >= base + size)
            {
                continue;
            }
            if (size >= 256u)
            {
                uint64_t const sub = (addr - base) / (size / 8u);
                if (((rasr >> (8u + sub)) & 1u) != 0u)
                {
                    continue;
                }
            }
            Hit hit;
            hit.ap = (rasr >> 24) & 0x7u;
            hit.xn = ((rasr >> 28) & 1u) != 0u;
            hit.tex_c_b = (rasr >> 16) & 0x3Bu;
            uint32_t const tex = (rasr >> 19) & 0x7u;
            uint32_t const cb = (rasr >> 16) & 0x3u;
            if (tex == 0u and cb == 0u)
            {
                hit.kind = Kind::strongly_ordered;
            }
            else if ((tex == 0u and cb == 1u) or (tex == 2u and cb == 0u))
            {
                hit.kind = Kind::device;
            }
            else
            {
                hit.kind = Kind::normal;
            }
            return hit;
        }
        return Hit{};
    }

    constexpr uint32_t AP_NONE = 0u;
    constexpr uint32_t AP_PRIV_RW = 1u;
    constexpr uint32_t AP_PRIV_RO = 5u;
    constexpr uint32_t WBWA = (1u << 3) | 0x3u;
    constexpr uint32_t WB = 0x3u;
}

TEST(ImxrtMpuRows, every_unbacked_address_is_denied_and_never_normal)
{
    constexpr uint32_t holes[] = {
        0x00000000u, // ITCM, which no image places anything in
        0x00200000u, // ROMCP
        0x00400000u, 0x08000000u, 0x0FFFFFFCu, 0x10000000u, 0x1FFFFFFCu,
        0x20000000u, // DTCM, which no image places anything in
        0x20100000u, 0x201FFFFCu,
        0x20280000u, // FlexRAM OCRAM, which no image places anything in
        0x20300000u, 0x30000000u, 0x3FFFFFFCu,
        0x40800000u, 0x41000000u, 0x41800000u, 0x42800000u, 0x44000000u, 0x50000000u, 0x5FFFFFFCu,
        FLASH_BASE + FLASH_SIZE, 0x6FFFFFFCu, 0x70000000u, 0x7F000000u, 0x7FFFFFFCu,
        0x80000000u, 0x9FFFFFFCu, 0xA0000000u, 0xC0000000u, 0xDFFFFFFCu,
    };
    for (uint32_t addr : holes)
    {
        Hit const hit = resolve(addr);
        EXPECT_NE(hit.kind, Kind::unmatched) << std::hex << addr << " falls to the default map";
        EXPECT_NE(hit.kind, Kind::normal) << std::hex << addr;
        EXPECT_EQ(hit.ap, AP_NONE) << std::hex << addr;
        EXPECT_TRUE(hit.xn) << std::hex << addr;
    }
}

TEST(ImxrtMpuRows, the_image_memory_keeps_its_privileged_access_and_type)
{
    for (uint32_t addr : {FLASH_BASE, FLASH_BASE + FLASH_SIZE - 4u})
    {
        Hit const hit = resolve(addr);
        EXPECT_EQ(hit.kind, Kind::normal) << std::hex << addr;
        EXPECT_EQ(hit.tex_c_b, WB) << std::hex << addr;
        EXPECT_EQ(hit.ap, AP_PRIV_RO) << std::hex << addr;
        EXPECT_FALSE(hit.xn) << std::hex << addr;
    }
    for (uint32_t addr : {OCRAM2_BASE, OCRAM2_BASE + OCRAM2_SIZE - 4u})
    {
        Hit const hit = resolve(addr);
        EXPECT_EQ(hit.kind, Kind::normal) << std::hex << addr;
        EXPECT_EQ(hit.tex_c_b, WBWA) << std::hex << addr;
        EXPECT_EQ(hit.ap, AP_PRIV_RW) << std::hex << addr;
        EXPECT_TRUE(hit.xn) << std::hex << addr;
    }
    // AIPS-1, USB1 on AIPS-3, the last word of AIPS-4, then AIPS-5.
    for (uint32_t addr : {0x40000000u, 0x402E0000u, 0x403FFFFCu, 0x42000000u, 0x420FFFFCu})
    {
        Hit const hit = resolve(addr);
        EXPECT_EQ(hit.kind, Kind::device) << std::hex << addr;
        EXPECT_EQ(hit.ap, AP_PRIV_RW) << std::hex << addr;
        EXPECT_TRUE(hit.xn) << std::hex << addr;
    }
}

TEST(ImxrtMpuRows, no_row_grants_unprivileged_access)
{
    for (size_t i = 0; i < mr::COUNT; i++)
    {
        uint32_t const ap = (TABLE.row[i].rasr >> 24) & 0x7u;
        EXPECT_TRUE(ap == AP_NONE or ap == AP_PRIV_RW or ap == AP_PRIV_RO) << "row " << i;
    }
}

TEST(ImxrtMpuRows, the_rows_leave_a_full_per_thread_set)
{
    EXPECT_LE(mr::COUNT + ARCH_MPU_ENCODED_SLOTS, HW_REGIONS);
}
