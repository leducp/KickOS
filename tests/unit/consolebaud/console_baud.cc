// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Every chip port's console baud divisor against its reference manual, at the nominal rate
// and at each fallback rate the port can land on.

#include "arch/arm/chip/mk64f/regs/uart.h"
#include "arch/arm/chip/rp2xxx/pl011_baud.h"
#include "arch/arm/chip/sam3x8e/uart_baud.h"
#include "arch/arm/chip/xmc4800/regs/usic.h"
#include "arch/arm/common/stm32_usart.h"
#include "arch/rx/chip/rx72m/regs/sci.h"
#include "arch/xtensa/chip/esp32/regs/uart.h"

#include <gtest/gtest.h>

#include <stdint.h>

namespace
{
    constexpr uint32_t BAUD = 115200u;
}

// RM0008 / RM0365 / RM0383, OVER8=0: BRR = USARTDIV x 16 = fck / baud, rounded to nearest.
TEST(ConsoleBaud, stm32_usart_brr)
{
    struct Case
    {
        uint32_t fck;
        uint32_t brr;
        char const* what;
    };
    Case const cases[] = {
        {72000000u, 625u, "F103 PCLK2 on the PLL"},
        {8000000u, 69u, "F103 and F302 on the HSI fallback"},
        {32000000u, 278u, "F302 PCLK1 on the PLL"},
        {42000000u, 365u, "F411 PCLK1 on the PLL"},
        {16000000u, 139u, "F411 on the HSI fallback"},
        {BAUD * 100u + BAUD / 2u, 101u, "exactly half rounds up"},
        {BAUD * 100u + BAUD / 2u - 1u, 100u, "just under half rounds down"},
    };
    for (Case const& c : cases)
    {
        EXPECT_EQ(kickos::stm32::usart_brr(c.fck, BAUD), c.brr) << c.what;
    }
}

// K64 RM 52.4.3: baud = clk / (16 x (SBR + BRFA/32)), the 1/32 remainder truncated.
TEST(ConsoleBaud, mk64f_sbr_brfa)
{
    namespace u = kickos::mk64f::reg::uart;
    struct Case
    {
        uint32_t clk;
        uint32_t sbr;
        uint32_t brfa;
        char const* what;
    };
    Case const cases[] = {
        {120000000u, 65u, 3u, "PEE, the nominal core clock"},
        {20971520u, 11u, 12u, "FEI, the fallback"},
        {16u * BAUD * 66u - 1u, 65u, 31u, "a remainder of 31.99/32 stays 31"},
        {16u * BAUD * 66u, 66u, 0u, "and the next clock carries into SBR"},
    };
    for (Case const& c : cases)
    {
        EXPECT_EQ(u::baud_sbr(c.clk, BAUD), c.sbr) << c.what;
        EXPECT_EQ(u::baud_brfa(c.clk, BAUD), c.brfa) << c.what;
    }
}

// RP2040 DS 4.2.3.2, RP2350 DS 12.1.3.2: baud = clk_peri / (16 x (IBRD + FBRD/64)),
// FBRD = round(fraction x 64).
TEST(ConsoleBaud, rp2xxx_pl011_ibrd_fbrd)
{
    struct Case
    {
        uint32_t clk;
        uint32_t ibrd;
        uint32_t fbrd;
        char const* what;
    };
    Case const cases[] = {
        {12000000u, 6u, 33u, "both parts on the XOSC fallback"},
        {125000000u, 67u, 52u, "RP2040 clk_sys on the PLL"},
        {150000000u, 81u, 24u, "RP2350 clk_sys on the PLL"},
        {12024000u, 6u, 34u, "exactly half a 64th rounds up"},
        {12023999u, 6u, 33u, "just under half a 64th rounds down"},
        {12893760u, 7u, 0u, "a fraction rounding to 64/64 carries into IBRD"},
    };
    for (Case const& c : cases)
    {
        EXPECT_EQ(kickos::rp2xxx::pl011_ibrd(c.clk, BAUD), c.ibrd) << c.what;
        EXPECT_EQ(kickos::rp2xxx::pl011_fbrd(c.clk, BAUD), c.fbrd) << c.what;
    }
}

// SAM3X datasheet sec.34: CD = MCK / (16 x baud), rounded; CD 0 would stop the generator.
TEST(ConsoleBaud, sam3x8e_uart_brgr_cd)
{
    struct Case
    {
        uint32_t mck;
        uint32_t cd;
        char const* what;
    };
    Case const cases[] = {
        {84000000u, 46u, "PLLA/2, the nominal MCK"},
        {12000000u, 7u, "the crystal, PLLA never locked"},
        {6000000u, 3u, "the crystal halved, the PLL switch stopped half way"},
        {4000000u, 2u, "the RC oscillator, the crystal never started"},
        {16u * BAUD * 21u / 2u, 11u, "exactly half rounds up"},
        {16u * BAUD * 21u / 2u - 1u, 10u, "just under half rounds down"},
        {900000u, 1u, "a quotient that rounds to 0 is held at 1"},
    };
    for (Case const& c : cases)
    {
        EXPECT_EQ(kickos::sam3x8e::uart_brgr_cd(c.mck, BAUD), c.cd) << c.what;
    }
}

// XMC4800 RM eq.18.6: fASC = fPERIPH x STEP/1024 / ((PDIV+1) x (PCTQ+1) x (DCTQ+1)). The
// divisors are solved off-line per fPERIPH, so the check is that each lands on the baud.
TEST(ConsoleBaud, xmc4800_usic_baud)
{
    namespace u = kickos::xmc::reg::usic;
    struct Case
    {
        uint32_t periph_hz;
        uint32_t max_error_ppm;
        char const* what;
    };
    Case const cases[] = {
        {72000000u, 10u, "fCPU 144 MHz on the PLL, PBDIV /2"},
        {60000000u, 40u, "60 MHz"},
        {48000000u, 300u, "the 96 MHz clock-select point"},
        {24000000u, 550u, "fOFI, the crystal or PLL never came up, and the 48 MHz point"},
    };
    for (Case const& c : cases)
    {
        u::Baud b = {};
        ASSERT_TRUE(u::baud_115200_for(c.periph_hz, &b)) << c.what;
        double const got = static_cast<double>(c.periph_hz) * b.step / 1024.0
                           / ((b.pdiv + 1.0) * (b.pctq + 1.0) * (b.dctq + 1.0));
        double const ppm = (got - BAUD) / BAUD * 1e6;
        EXPECT_LE(ppm, static_cast<double>(c.max_error_ppm)) << c.what << ": " << got;
        EXPECT_GE(ppm, -static_cast<double>(c.max_error_ppm)) << c.what << ": " << got;
    }

    u::Baud b = {};
    EXPECT_FALSE(u::baud_115200_for(144000000u, &b)) << "fCPU is not a USIC clock";
    EXPECT_FALSE(u::baud_115200_for(0u, &b)) << "no clock has no divisor";
}

// ESP32 TRM Register 19.6: baud = APB / (CLKDIV + FRAG/16), the sixteenths truncated.
TEST(ConsoleBaud, esp32_uart_clkdiv)
{
    struct Case
    {
        uint32_t apb;
        uint32_t word;
        char const* what;
    };
    Case const cases[] = {
        {80000000u, (7u << 20) | 694u, "APB once the CPU is on the PLL"},
        {40000000u, (3u << 20) | 347u, "APB as the ROM leaves it"},
        {(BAUD * (16u * 694u + 15u) + BAUD - 1u) / 16u, (15u << 20) | 694u,
         "15.99 sixteenths stay 15"},
    };
    for (Case const& c : cases)
    {
        EXPECT_EQ(kickos::esp32::reg::uart::clkdiv(c.apb, BAUD), c.word) << c.what;
    }
}

// RX72M UM Table 42.11: B = PCLK / (D x (N + 1)), D set by SMR.CKS and SEMR.BGDM/ABCS.
TEST(ConsoleBaud, rx72m_baud_select)
{
    namespace s = kickos::rx::reg::sci;
    constexpr uint8_t BGDM = s::SEMR_BGDM;
    constexpr uint8_t BGDM_ABCS = s::SEMR_BGDM | s::SEMR_ABCS;
    struct Case
    {
        uint32_t clk;
        uint32_t baud;
        uint8_t cks;
        uint8_t semr;
        uint8_t brr;
        char const* what;
    };
    Case const cases[] = {
        {60000000u, BAUD, 0u, BGDM_ABCS, 64u, "PCLKB on the PLL"},
        {20000000u, BAUD, 0u, BGDM, 10u, "the ceiling N wins when the quotient is past a half"},
        {15360000u, 9600u, 0u, BGDM, 99u, "an exact tie goes to 16 base clocks per bit"},
    };
    for (Case const& c : cases)
    {
        s::BaudSetting bs = {};
        ASSERT_TRUE(s::baud_select(c.clk, c.baud, &bs)) << c.what;
        EXPECT_EQ(bs.cks, c.cks) << c.what;
        EXPECT_EQ(bs.semr, c.semr) << c.what;
        EXPECT_EQ(bs.brr, c.brr) << c.what;
    }

    struct Refusal
    {
        uint32_t clk;
        uint32_t baud;
        char const* what;
    };
    Refusal const refusals[] = {
        {240000u, BAUD, "the 240 kHz LOCO, the PLL never came up"},
        {60000000u, 4000000u, "nothing within the tolerance"},
        {0u, BAUD, "no clock"},
        {60000000u, 0u, "no baud"},
    };
    for (Refusal const& r : refusals)
    {
        s::BaudSetting bs = {};
        EXPECT_FALSE(s::baud_select(r.clk, r.baud, &bs)) << r.what;
    }
}
