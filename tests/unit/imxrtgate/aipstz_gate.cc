// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// USB1's AIPSTZ slot opened and closed for its holder, over a register file holding every OPACR
// word of the four bridges. The expected word and bit are the reference manual's, not the
// header's: OTG1 is AIPS-3 slot 24, OPACR3's top nibble, whose Supervisor-Protect bit is the
// nibble's bit 2.

#include "arch/arm/chip/imxrt1062/aipstz_gate.h"

#include <gtest/gtest.h>

#include <stdint.h>

namespace
{
    namespace gate = kickos::imxrt1062::aipstz_gate;

    constexpr uintptr_t BRIDGES[] = {0x4007C000u, 0x4017C000u, 0x4027C000u, 0x4037C000u};
    constexpr uint32_t BRIDGE_COUNT = 4u;
    // OPACR, OPACR1 to OPACR4 (RM 32.8).
    constexpr uintptr_t OPACR_FIRST = 0x40u;
    constexpr uint32_t OPACR_WORDS = 5u;
    constexpr uint32_t WORDS = BRIDGE_COUNT * OPACR_WORDS;

    constexpr uintptr_t USB1 = 0x402E0000u;
    constexpr uintptr_t USB1_OPACR = 0x4027C04Cu;
    constexpr uint32_t USB1_SP = 1u << 30;
    constexpr uint32_t OPACR_RESET = 0x44444444u;

    uint32_t g_file[WORDS];
    uint32_t g_accesses = 0;
    uint32_t g_strays = 0;
    uint32_t g_sink = 0;

    uintptr_t address_of(uint32_t i)
    {
        return BRIDGES[i / OPACR_WORDS] + OPACR_FIRST + (i % OPACR_WORDS) * 4u;
    }

    volatile uint32_t& fake(uintptr_t address)
    {
        g_accesses++;
        for (uint32_t i = 0; i < WORDS; i++)
        {
            if (address_of(i) == address)
            {
                return g_file[i];
            }
        }
        g_strays++;
        return g_sink;
    }

    void fill(uint32_t value)
    {
        for (uint32_t& w : g_file)
        {
            w = value;
        }
        g_accesses = 0;
        g_strays = 0;
    }

    // Every word as `value` but USB1's, which holds `usb1`.
    void expect_file(uint32_t value, uint32_t usb1)
    {
        for (uint32_t i = 0; i < WORDS; i++)
        {
            uint32_t want = value;
            if (address_of(i) == USB1_OPACR)
            {
                want = usb1;
            }
            EXPECT_EQ(g_file[i], want) << "OPACR word at 0x" << std::hex << address_of(i);
        }
        EXPECT_EQ(g_strays, 0u) << "an access outside the OPACR words";
    }

    TEST(AipstzGate, open_clears_usb1s_protect_bit_and_close_sets_it_back)
    {
        fill(OPACR_RESET);
        EXPECT_EQ(gate::open(USB1, fake), 0);
        expect_file(OPACR_RESET, OPACR_RESET & ~USB1_SP);
        EXPECT_EQ(gate::close(USB1, fake), 0);
        expect_file(OPACR_RESET, OPACR_RESET);
        EXPECT_EQ(gate::close(USB1, fake), 0) << "a second close";
        expect_file(OPACR_RESET, OPACR_RESET);
        EXPECT_EQ(gate::open(USB1, fake), 0) << "a reopen";
        expect_file(OPACR_RESET, OPACR_RESET & ~USB1_SP);
    }

    TEST(AipstzGate, each_half_moves_usb1s_protect_bit_alone)
    {
        fill(0xFFFFFFFFu);
        EXPECT_EQ(gate::open(USB1, fake), 0);
        expect_file(0xFFFFFFFFu, 0xFFFFFFFFu & ~USB1_SP);
        fill(0u);
        EXPECT_EQ(gate::close(USB1, fake), 0);
        expect_file(0u, USB1_SP);
    }

    TEST(AipstzGate, a_base_with_no_entry_is_refused_and_touches_nothing)
    {
        uintptr_t const others[] = {0u, USB1 + 0x200u, USB1 + 0x800u, USB1 + 4u, USB1 - 0x4000u,
                                    0x4027C000u, 0x40184000u};
        for (uintptr_t const base : others)
        {
            fill(OPACR_RESET);
            EXPECT_EQ(gate::open(base, fake), -KOS_EINVAL) << std::hex << base;
            EXPECT_EQ(gate::close(base, fake), -KOS_EINVAL) << std::hex << base;
            EXPECT_EQ(g_accesses, 0u) << "a refused base reached a register";
        }
    }
}
