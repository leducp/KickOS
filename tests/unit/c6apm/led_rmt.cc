// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The ESP32-C6 addressable diagnostic LED (arch/riscv/chip/esp32c6/led_rmt.h) over a fake register
// file: the RMT may be a task's until the terminal panic, so the init takes the block's clock and
// reset and channel 0 back before the first frame, and touches no other channel. The RMT's PCR
// gates (arch/riscv/chip/esp32c6/rmt_clock.h), which a task's kos_periph_enable sets, too.

#include "arch/riscv/chip/esp32c6/led_rmt.h"

#include <gtest/gtest.h>

#include <stdint.h>

#include <map>
#include <utility>
#include <vector>

namespace
{
    namespace led = kickos::esp32c6::led_rmt;
    namespace reg = kickos::esp32c6::reg;
    namespace mmap = kickos::esp32c6::mmap;

    constexpr uint32_t PIN = 8;

    // Each access gets a slot primed with the register's value; the slot is stored back, in
    // access order, at the next access, so `log` holds every access with the value it left.
    struct FakeBus
    {
        std::map<uintptr_t, uint32_t> regs;
        std::vector<std::pair<uintptr_t, uint32_t>> log;
        std::vector<uint32_t> slots = std::vector<uint32_t>(4096);
        bool pending = false;
        uintptr_t pending_address = 0;

        void settle()
        {
            if (pending)
            {
                uint32_t const value = slots[log.size()];
                regs[pending_address] = value;
                log.emplace_back(pending_address, value);
                pending = false;
            }
        }
    };

    FakeBus g_bus;

    volatile uint32_t& fake(uintptr_t address)
    {
        g_bus.settle();
        size_t const at = g_bus.log.size();
        g_bus.slots.at(at) = g_bus.regs[address];
        g_bus.pending = true;
        g_bus.pending_address = address;
        return g_bus.slots[at];
    }

    bool in_rmt(uintptr_t a)
    {
        return a >= mmap::RMT_BASE and a < mmap::RMT_BASE + 0x1000u;
    }

    class LedRmt : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            g_bus = FakeBus{};
            // A task left channel 0 transmitting with its carrier on, the block's clock gated
            // and the RMT held in reset.
            g_bus.regs[reg::pcr::RMT_CONF] = reg::pcr::RMT_RST_EN;
            g_bus.regs[reg::rmt::CH0CONF0] = reg::rmt::CARRIER_EN | reg::rmt::TX_START;
            g_bus.regs[reg::rmt::INT_RAW] = reg::rmt::CH0_TX_END;
            led::init(PIN);
            led::send(0xFF0000u);
            g_bus.settle();
        }

        size_t first(uintptr_t address, uint32_t bits) const
        {
            for (size_t i = 0; i < g_bus.log.size(); i++)
            {
                if (g_bus.log[i].first == address and (g_bus.log[i].second & bits) == bits)
                {
                    return i;
                }
            }
            return g_bus.log.size();
        }

        size_t last(uintptr_t address) const
        {
            size_t at = g_bus.log.size();
            for (size_t i = 0; i < g_bus.log.size(); i++)
            {
                if (g_bus.log[i].first == address)
                {
                    at = i;
                }
            }
            return at;
        }
    };

    TEST_F(LedRmt, the_block_is_clocked_and_out_of_reset_before_the_first_frame_starts)
    {
        size_t const tx = first(reg::rmt::CH0CONF0, reg::rmt::TX_START);
        ASSERT_LT(tx, g_bus.log.size());
        EXPECT_LT(first(reg::pcr::RMT_CONF, reg::pcr::RMT_RST_EN | reg::pcr::RMT_CLK_EN), tx);
        EXPECT_LT(last(reg::pcr::RMT_CONF), tx);
        EXPECT_EQ(g_bus.regs[reg::pcr::RMT_CONF], reg::pcr::RMT_CLK_EN);
        EXPECT_NE(g_bus.regs[reg::pcr::RMT_SCLK_CONF] & reg::pcr::RMT_SCLK_EN, 0u);
    }

    TEST_F(LedRmt, channel_0_is_configured_before_the_first_frame_and_its_carrier_is_off)
    {
        size_t const tx = first(reg::rmt::CH0CONF0, reg::rmt::TX_START);
        size_t const conf = first(reg::rmt::CH0CONF0, led::CH0_CFG | reg::rmt::CONF_UPDATE);
        EXPECT_LT(conf, tx);
        EXPECT_LT(first(reg::rmt::SYS_CONF, reg::rmt::APB_FIFO_MASK), tx);
        EXPECT_EQ(g_bus.log[conf].second & reg::rmt::CARRIER_EN, 0u);
        EXPECT_LT(first(reg::gpio::func_out_sel_cfg(PIN), reg::gpio::RMT_SIG_OUT0_IDX), tx);
        EXPECT_LT(first(reg::gpio::ENABLE_W1TS, 1u << PIN), tx);
    }

    TEST(RmtClock, a_gated_block_held_in_reset_is_clocked_from_xtal_and_released)
    {
        g_bus = FakeBus{};
        g_bus.regs[reg::pcr::RMT_CONF] = reg::pcr::RMT_RST_EN;
        kickos::esp32c6::rmt_clock::enable();
        g_bus.settle();
        EXPECT_EQ(g_bus.regs[reg::pcr::RMT_CONF], reg::pcr::RMT_CLK_EN);
        uint32_t const sclk = g_bus.regs[reg::pcr::RMT_SCLK_CONF];
        EXPECT_NE(sclk & reg::pcr::RMT_SCLK_EN, 0u);
        EXPECT_EQ((sclk >> reg::pcr::RMT_SCLK_SEL_S) & 3u, 3u);
    }

    TEST(RmtClock, enabling_a_running_block_never_resets_it)
    {
        g_bus = FakeBus{};
        g_bus.regs[reg::pcr::RMT_CONF] = reg::pcr::RMT_CLK_EN;
        kickos::esp32c6::rmt_clock::enable();
        kickos::esp32c6::rmt_clock::enable();
        g_bus.settle();
        for (auto const& [address, value] : g_bus.log)
        {
            if (address == reg::pcr::RMT_CONF)
            {
                EXPECT_EQ(value & reg::pcr::RMT_RST_EN, 0u);
            }
            EXPECT_FALSE(in_rmt(address)) << std::hex << address;
        }
        EXPECT_EQ(g_bus.regs[reg::pcr::RMT_CONF], reg::pcr::RMT_CLK_EN);
    }

    TEST_F(LedRmt, nothing_outside_channel_0_and_the_block_registers_is_touched)
    {
        for (auto const& [address, value] : g_bus.log)
        {
            if (not in_rmt(address))
            {
                continue;
            }
            bool const ram = address >= reg::rmt::CH0_RAM and address <= reg::rmt::CH0_RAM + 4u * led::BITS;
            bool const known = address == reg::rmt::CH0CONF0 or address == reg::rmt::INT_RAW
                               or address == reg::rmt::INT_CLR or address == reg::rmt::SYS_CONF;
            EXPECT_TRUE(ram or known) << std::hex << address;
            if (address == reg::rmt::INT_CLR)
            {
                EXPECT_EQ(value & ~reg::rmt::CH0_TX_END, 0u);
            }
        }
    }
}

volatile uint32_t& kickos::testfix::ledrmt::word(uintptr_t address)
{
    return fake(address);
}
