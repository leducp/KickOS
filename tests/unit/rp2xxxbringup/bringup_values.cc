// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Pins arch/arm/chip/rp2xxx/bringup_values.h at every clk_ref each part can land on, and the
// pad bits each part's pin function clears.

#include "arch/arm/chip/rp2xxx/bringup_values.h"

#include <gtest/gtest.h>

#include <stdint.h>

namespace
{
    using kickos::rp2xxx::pad_clear;
    using kickos::rp2xxx::PIN_DRIVE;
    using kickos::rp2xxx::PIN_INPUT;
    using kickos::rp2xxx::PIN_SIO_OUT;
    using kickos::rp2xxx::tick_cycles;

    // PADS_BANK0 GPIOn: OD is bit 7 on both parts, ISO bit 8 on the RP2350 alone. Each part's
    // family_map.h asserts its PAD_RELEASE against the row this test gives it.
    constexpr uint32_t OD = 1u << 7;
    constexpr uint32_t ISO = 1u << 8;
}

TEST(Rp2xxxBringup, TickCyclesAtEveryClkRef)
{
    struct Case
    {
        uint32_t ref_hz;
        uint32_t cycles;
        char const* what;
    };
    Case const cases[] = {
        {12000000u, 12u, "the crystal, both parts"},
        {6500000u, 7u, "the RP2040 ROSC fallback"},
        {11000000u, 11u, "the RP2350 ROSC fallback"},
        {6000000u, 6u, "the RP2350 crystal behind CLK_REF_DIV 2"},
        {12000000u / 256u, 1u, "a divisor that rounds to zero ticks"},
        {600000000u, 0x1ffu, "a rate past the 9-bit field"},
        {1500000u, 2u, "exactly half rounds up"},
        {1499999u, 1u, "just under half rounds down"},
    };
    for (Case const& c : cases)
    {
        EXPECT_EQ(tick_cycles(c.ref_hz), c.cycles) << c.what;
    }
}

TEST(Rp2xxxBringup, PadClearPerPart)
{
    struct Case
    {
        uint32_t func;
        uint32_t release;
        uint32_t clr;
        char const* what;
    };
    Case const cases[] = {
        {0x2u, 0u, 0u, "RP2040, a funcsel alone clears nothing"},
        {0x2u | PIN_INPUT, 0u, 0u, "RP2040, input only"},
        {0x2u | PIN_DRIVE, 0u, OD, "RP2040, driven"},
        {0x5u | PIN_DRIVE | PIN_SIO_OUT, 0u, OD, "RP2040, SIO output"},
        {0x2u, ISO, ISO, "RP2350, a funcsel alone still releases the pad"},
        {0x2u | PIN_INPUT, ISO, ISO, "RP2350, input only"},
        {0x2u | PIN_DRIVE, ISO, ISO | OD, "RP2350, driven"},
        {0x1fu | PIN_DRIVE | PIN_INPUT | PIN_SIO_OUT, ISO, ISO | OD, "RP2350, every bit"},
    };
    for (Case const& c : cases)
    {
        EXPECT_EQ(pad_clear(c.func, c.release, OD), c.clr) << c.what;
    }
}

// The func word is KOS_SYS_PINMUX_SET's ABI on both parts.
TEST(Rp2xxxBringup, PinFuncEncoding)
{
    EXPECT_EQ(kickos::rp2xxx::PIN_FUNCSEL_MASK, 0x1fu);
    EXPECT_EQ(PIN_INPUT, 1u << 8);
    EXPECT_EQ(PIN_DRIVE, 1u << 9);
    EXPECT_EQ(PIN_SIO_OUT, 1u << 16);
}
