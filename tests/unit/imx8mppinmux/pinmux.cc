// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A driver granted a non-console UART muxes that UART's pads; the console's pads and an unlisted
// pad stay refused, and a call writes only the named pad's registers.

#include "arch/arm64/chip/imx8mp/pinmux.h"

#include <gtest/gtest.h>

#include <stdint.h>

namespace
{
    namespace pm = kickos::imx8mp::pinmux;

    constexpr uint32_t UART2_RXD = 24u;
    constexpr uint32_t UART2_TXD = 25u;
    constexpr uint32_t UART4_RXD = 28u;
    constexpr uint32_t ALT0 = 0u;
    constexpr uint32_t SION = 1u << 4;

    pm::Plan grant(uint32_t bit, uint32_t func)
    {
        return pm::plan(5u, bit, func, true, false);
    }
}

TEST(Imx8mpPinmux, AGrantedUartsRxdPadTakesItsMuxPadControlAndInputSelect)
{
    uint32_t const pad = 0x1C6u; // PE, HYS, PUE, DSE x6
    uint32_t const func = ALT0 | SION | pm::PAD_WRITE | (pad << pm::PAD_SHIFT) | pm::DAISY_WRITE
                          | (6u << pm::DAISY_SHIFT);
    pm::Plan const p = grant(UART2_RXD, func);
    ASSERT_EQ(p.rc, 0);
    EXPECT_EQ(p.mux, 0x30330228u);
    EXPECT_EQ(p.mux_value, SION);
    EXPECT_EQ(p.pad, 0x30330488u);
    EXPECT_EQ(p.pad_value, pad);
    EXPECT_EQ(p.daisy, 0x303305F0u);
    EXPECT_EQ(p.daisy_value, 6u);
}

TEST(Imx8mpPinmux, AMuxAloneWritesNothingElse)
{
    pm::Plan const p = grant(UART2_TXD, ALT0);
    ASSERT_EQ(p.rc, 0);
    EXPECT_EQ(p.mux, 0x3033022Cu);
    EXPECT_EQ(p.pad, 0u);
    EXPECT_EQ(p.daisy, 0u);
}

TEST(Imx8mpPinmux, TheKernelsPadsAreBusy)
{
    EXPECT_EQ(pm::plan(5u, 22u, ALT0, true, true).rc, -KOS_EBUSY);
}

TEST(Imx8mpPinmux, AnUnlistedPadIsRefused)
{
    EXPECT_EQ(pm::plan(5u, 30u, ALT0, true, false).rc, -KOS_EINVAL);
    EXPECT_EQ(pm::plan(4u, 24u, ALT0, true, false).rc, -KOS_EINVAL);
    EXPECT_EQ(pm::plan(5u, 24u, ALT0, false, false).rc, -KOS_EINVAL);
}

TEST(Imx8mpPinmux, ABitNoFieldTakesIsRefused)
{
    EXPECT_EQ(grant(UART2_RXD, 1u << 3).rc, -KOS_EINVAL);
    EXPECT_EQ(grant(UART2_RXD, 1u << 20).rc, -KOS_EINVAL);
    EXPECT_EQ(grant(UART2_RXD, pm::PAD_WRITE | (1u << pm::PAD_SHIFT)).rc, -KOS_EINVAL);
    // DAISY is bits [2:0] of UART1 to UART3's input select (8.2.4.374 to 8.2.4.378).
    EXPECT_EQ(grant(UART2_RXD, pm::DAISY_WRITE | (8u << pm::DAISY_SHIFT)).rc, -KOS_EINVAL);
}

TEST(Imx8mpPinmux, Uart4sInputSelectIsFourBitsWide)
{
    // UART4_RXD at ALT0 is DAISY 1000b (8.2.4.380).
    pm::Plan const p = grant(UART4_RXD, ALT0 | pm::DAISY_WRITE | (8u << pm::DAISY_SHIFT));
    ASSERT_EQ(p.rc, 0);
    EXPECT_EQ(p.daisy, 0x30330600u);
    EXPECT_EQ(p.daisy_value, 8u);
}

TEST(Imx8mpPinmux, ATxdPadHasNoInputSelect)
{
    EXPECT_EQ(grant(UART2_TXD, pm::DAISY_WRITE | (6u << pm::DAISY_SHIFT)).rc, -KOS_EINVAL);
}
