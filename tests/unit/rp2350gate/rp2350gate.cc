// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The RP2350 partition gate's rows checked before any is written, and the register image each
// one gives its node's core.

#include "arch/arm/chip/rp2350/accessctrl_rows.h"

#include <gtest/gtest.h>

#include <stdint.h>

namespace
{
    namespace accessctrl = kickos::rp2350::accessctrl;
    namespace gate = kickos::rp2350::reg::accessctrl;

    constexpr uint8_t RW = KICKOS_GATE_R | KICKOS_GATE_W;
    constexpr uint32_t NODE_CORE[] = {0u, 1u};
    constexpr uint32_t NODES = 2u;
    // Table 911's reset value of UART0's register: DBG, DMA, CORE1, CORE0, SP and SU.
    constexpr uint32_t UART0_RESET = 0xFCu;

    kickos_gate_row row(uint16_t reg, uint8_t node)
    {
        return {0x40070000ull, 0x4000ull, 0, reg, node, RW, 0};
    }

    accessctrl::Refusal refusal(kickos_gate_row const* rows, uint16_t count, uint16_t& at)
    {
        return accessctrl::refusal_of(rows, count, NODE_CORE, NODES, at);
    }
}

TEST(Rp2350Gate, AmppingIsAdmitted)
{
    kickos_gate_row const rows[] = {row(0xA0u, 0u)};
    uint16_t at = 0;
    EXPECT_EQ(refusal(rows, 1u, at), accessctrl::Refusal::NONE);
}

TEST(Rp2350Gate, ImageGivesTheNodesCoreAloneAndKeepsDma)
{
    EXPECT_EQ(accessctrl::image_of(UART0_RESET, 0u), 0xACCE00DCu);
    EXPECT_EQ(accessctrl::image_of(UART0_RESET, 1u), 0xACCE00ECu);
    EXPECT_EQ(accessctrl::image_of(UART0_RESET & ~gate::DMA, 0u), 0xACCE009Cu);
}

TEST(Rp2350Gate, RefusesARegisterNoNodeIsAssigned)
{
    uint16_t const never[] = {0x14u, 0x18u, 0x1Cu, 0x2Cu, 0x40u, 0x44u, 0x64u, 0x68u, 0x70u, 0x98u,
                              0xA4u, 0xC0u, 0xC4u, 0xCCu, 0xD0u, 0xD4u, 0xD8u, 0xE0u, 0xE4u, 0xE8u};
    for (uint16_t reg : never)
    {
        kickos_gate_row const rows[] = {row(0xA0u, 0u), row(reg, 1u)};
        uint16_t at = 0;
        EXPECT_EQ(refusal(rows, 2u, at), accessctrl::Refusal::NEVER) << reg;
        EXPECT_EQ(at, 1u) << reg;
    }
}

TEST(Rp2350Gate, RefusesARegisterTwoRowsName)
{
    kickos_gate_row const rows[] = {row(0xA0u, 0u), row(0x48u, 0u), row(0xA0u, 1u)};
    uint16_t at = 0;
    EXPECT_EQ(refusal(rows, 3u, at), accessctrl::Refusal::TWICE);
    EXPECT_EQ(at, 2u);
}

TEST(Rp2350Gate, RefusesWhatIsNoBusRegisterOrCore)
{
    uint16_t at = 0;
    kickos_gate_row bad[] = {row(0x10u, 0u)};
    EXPECT_EQ(refusal(bad, 1u, at), accessctrl::Refusal::REGISTER);
    bad[0] = row(0xECu, 0u);
    EXPECT_EQ(refusal(bad, 1u, at), accessctrl::Refusal::REGISTER);
    bad[0] = row(0xA2u, 0u);
    EXPECT_EQ(refusal(bad, 1u, at), accessctrl::Refusal::REGISTER);
    bad[0] = row(0xA0u, 2u);
    EXPECT_EQ(refusal(bad, 1u, at), accessctrl::Refusal::NODE);
    uint32_t const far_core[] = {0u, 2u};
    bad[0] = row(0xA0u, 1u);
    EXPECT_EQ(accessctrl::refusal_of(bad, 1u, far_core, NODES, at), accessctrl::Refusal::NODE);
}
