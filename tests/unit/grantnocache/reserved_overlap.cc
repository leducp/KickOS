// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The Rule 7 predicates over the REAL kernel/grant/grant.cc and a reserved table planted here:
// every edge of the overlap matrix, two flush blocks among them, the bit-band images, and the
// RAM and device admission a reserved block and the arena decide.

#include <kickos/grant.h>

#include <gtest/gtest.h>

#include <stddef.h>
#include <stdint.h>

#include "grant_seam.h"

namespace
{
    // Two blocks flush against each other, and a third apart above them, all inside the 1 MB
    // bit-band peripheral region.
    constexpr uintptr_t LOW = 0x40010000u;
    constexpr size_t LOW_SIZE = 0x400u;
    constexpr uintptr_t FLUSH = LOW + LOW_SIZE;
    constexpr size_t FLUSH_SIZE = 0x400u;
    constexpr uintptr_t HIGH = 0x40020000u;
    constexpr size_t HIGH_SIZE = 0x100u;
    constexpr uintptr_t HIGH_END = HIGH + HIGH_SIZE;
    constexpr arch_reserved_block BLOCKS[] = {
        {HIGH, HIGH_SIZE}, {LOW, LOW_SIZE}, {FLUSH, FLUSH_SIZE}};

    constexpr uintptr_t PERI_BASE = 0x40000000u;
    constexpr uintptr_t PERI_END = 0x40100000u;
    constexpr uintptr_t PERI_ALIAS = 0x42000000u;
    constexpr uintptr_t SRAM_ALIAS = 0x22000000u;
    constexpr uintptr_t BITS_PER_WORD = 32u;

    constexpr uintptr_t alias_of(uintptr_t peripheral)
    {
        return PERI_ALIAS + (peripheral - PERI_BASE) * BITS_PER_WORD;
    }

    // A block just past the peripheral region: it has no bit-band image, so the address its
    // formula would give names nothing.
    constexpr uintptr_t PAST_PERI = PERI_END;
    constexpr size_t PAST_PERI_SIZE = 0x100u;
    constexpr arch_reserved_block BLOCKS_AND_PAST_PERI[] = {
        {HIGH, HIGH_SIZE}, {LOW, LOW_SIZE}, {FLUSH, FLUSH_SIZE}, {PAST_PERI, PAST_PERI_SIZE}};

    constexpr uintptr_t IN_ARENA = ARENA_BASE + 0x2000u;
    constexpr size_t IN_ARENA_SIZE = 0x100u;
    constexpr arch_reserved_block ARENA_BLOCK[] = {{IN_ARENA, IN_ARENA_SIZE}};

    constexpr uintptr_t DEVICE = PERI_BASE;
    constexpr size_t DEVICE_SIZE = 0x1000u;
    constexpr uintptr_t WRAPPING = UINTPTR_MAX - 0xFu;
    constexpr size_t WRAP_SIZE = 0x20u;

    constexpr uint32_t RW = ARCH_MPU_R | ARCH_MPU_W;
    constexpr uint32_t DEV = ARCH_MPU_R | ARCH_MPU_W | ARCH_MPU_DEV;

    class ReservedOverlap : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            g_reserved = arch_reserved_span(BLOCKS);
        }

        void TearDown() override
        {
            g_reserved = arch_reserved_span();
            g_bitband = 0;
        }

        static bool hits(uintptr_t base, size_t size)
        {
            return kickos::grant_hits_reserved(base, size);
        }

        static bool admitted(uintptr_t base, size_t size, uint32_t attr, bool authorized)
        {
            return kickos::grant_region_admissible(base, size, attr, authorized);
        }
    };

    TEST_F(ReservedOverlap, a_grant_meeting_a_block_anywhere_hits_it)
    {
        EXPECT_TRUE(hits(LOW, LOW_SIZE)) << "equal";
        EXPECT_TRUE(hits(LOW + 4u, 8u)) << "contained";
        EXPECT_TRUE(hits(LOW - 4u, 8u)) << "straddling the low edge";
        EXPECT_TRUE(hits(LOW - 1u, 2u)) << "its last byte the block's first";
        EXPECT_TRUE(hits(HIGH_END - 1u, 2u)) << "its first byte the block's last";
        EXPECT_TRUE(hits(LOW, 1u)) << "the lowest block's first byte";
        EXPECT_TRUE(hits(HIGH_END - 1u, 1u)) << "the highest block's last byte";
    }

    TEST_F(ReservedOverlap, a_grant_beside_every_block_hits_none)
    {
        EXPECT_FALSE(hits(LOW - 0x10u, 0x10u)) << "adjacent below the lowest";
        EXPECT_FALSE(hits(HIGH_END, 0x10u)) << "adjacent above the highest";
        EXPECT_FALSE(hits(HIGH_END + 0x100000u, 0x10u)) << "well clear above";
        EXPECT_FALSE(hits(FLUSH + FLUSH_SIZE, HIGH - (FLUSH + FLUSH_SIZE)))
            << "the whole gap between the flush pair and the block above";
    }

    TEST_F(ReservedOverlap, two_flush_blocks_leave_no_gap_at_their_seam)
    {
        EXPECT_TRUE(hits(FLUSH - 1u, 2u)) << "across the seam";
        EXPECT_TRUE(hits(FLUSH, 1u)) << "the second block's first byte";
        EXPECT_TRUE(hits(FLUSH - 1u, 1u)) << "the first block's last byte";
    }

    TEST_F(ReservedOverlap, an_empty_grant_touches_nothing_and_a_wrapping_one_fails_closed)
    {
        EXPECT_FALSE(hits(LOW, 0u));
        EXPECT_TRUE(hits(WRAPPING, WRAP_SIZE));
    }

    TEST_F(ReservedOverlap, a_peripheral_block_is_hit_through_its_bit_band_image)
    {
        uintptr_t const image_end = alias_of(HIGH_END);
        EXPECT_FALSE(hits(alias_of(LOW), 4u)) << "a chip with no bit-band";
        g_bitband = 1;
        EXPECT_TRUE(hits(alias_of(LOW), 4u));
        EXPECT_TRUE(hits(image_end - 4u, 4u)) << "the highest block's image, last word";
        EXPECT_FALSE(hits(image_end, 4u)) << "the word past it";
        EXPECT_FALSE(hits(alias_of(LOW) - 4u, 4u)) << "the word before the lowest's image";
    }

    TEST_F(ReservedOverlap, a_block_outside_the_peripheral_region_has_no_bit_band_image)
    {
        g_reserved = arch_reserved_span(BLOCKS_AND_PAST_PERI);
        g_bitband = 1;
        EXPECT_TRUE(hits(PAST_PERI, PAST_PERI_SIZE)) << "the block itself";
        EXPECT_FALSE(hits(alias_of(PAST_PERI), 4u));
    }

    TEST_F(ReservedOverlap, a_device_window_on_a_bit_band_alias_is_refused)
    {
        EXPECT_TRUE(admitted(PERI_ALIAS, DEVICE_SIZE, DEV, true)) << "a chip with no bit-band";
        EXPECT_TRUE(admitted(SRAM_ALIAS, DEVICE_SIZE, DEV, true)) << "a chip with no bit-band";
        g_bitband = 1;
        EXPECT_FALSE(admitted(PERI_ALIAS, DEVICE_SIZE, DEV, true)) << "the peripheral alias";
        EXPECT_FALSE(admitted(SRAM_ALIAS, DEVICE_SIZE, DEV, true)) << "the SRAM alias";
    }

    TEST_F(ReservedOverlap, a_reserved_block_is_no_device_window)
    {
        EXPECT_FALSE(admitted(LOW, LOW_SIZE, DEV, true));
        EXPECT_TRUE(admitted(HIGH_END, HIGH_SIZE, DEV, true)) << "the device beside it";
    }

    TEST_F(ReservedOverlap, a_reserved_block_inside_the_arena_is_no_ram_grant)
    {
        g_reserved = arch_reserved_span(ARENA_BLOCK);
        EXPECT_FALSE(admitted(IN_ARENA, IN_ARENA_SIZE, RW, true));
        EXPECT_FALSE(admitted(IN_ARENA, IN_ARENA_SIZE, RW, false));
        EXPECT_TRUE(admitted(IN_ARENA + IN_ARENA_SIZE, IN_ARENA_SIZE, RW, false))
            << "the region beside it";
    }

    TEST_F(ReservedOverlap, a_device_grant_needs_an_authorised_caller)
    {
        EXPECT_FALSE(admitted(DEVICE, DEVICE_SIZE, DEV, false));
        EXPECT_TRUE(admitted(DEVICE, DEVICE_SIZE, DEV, true));
    }

    TEST_F(ReservedOverlap, a_ram_grant_is_one_region_inside_the_arena_for_every_caller)
    {
        uintptr_t const block = ARENA_BASE + 0x1000u;
        uintptr_t const below = 0x1000u;
        uintptr_t const past_top = ARENA_BASE + ARENA_SIZE;
        EXPECT_TRUE(admitted(block, ARENA_REGION, RW, true));
        EXPECT_TRUE(admitted(block, ARENA_REGION, RW, false));
        EXPECT_FALSE(admitted(block + 1u, ARENA_REGION, RW, true)) << "a base inside a region";
        EXPECT_FALSE(admitted(below, below, RW, true)) << "below the arena";
        EXPECT_FALSE(admitted(past_top, ARENA_REGION, RW, true)) << "past the arena's top";
        EXPECT_FALSE(admitted(WRAPPING, WRAP_SIZE, RW, true)) << "wrapping";
        EXPECT_FALSE(admitted(ARENA_BASE, 0u, RW, true)) << "empty";
    }
}
