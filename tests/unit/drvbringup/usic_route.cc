// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The XMC4800 USIC engines route their events onto the service request their config's line index
// names: the SSC engine (system/driver/xmc4800/xmcssc/spi_usic.cc) its receive events, the UART
// backend (system/driver/xmc4800/uart_usic.cc) its transmit-buffer event. Each opens a host buffer
// as its window, prefilled with a sentinel, over the fakes below.

#include <kickos/driver/spi.h>
#include <kickos/driver/uart.h>
#include <kickos/sys.h>
#include <kickos/sys/errno.h>
#include <kickos/sys/uart.h>

#include <regs/usic.h>
#include <usic_class.h>

#include <gtest/gtest.h>

#include <stdint.h>
#include <string.h>

namespace ru = kickos::xmc::reg::usic;

namespace
{
    constexpr uint32_t SENTINEL = 0xA5A5A5A5u;
    constexpr uint32_t PERIPH_HZ = 72000000u;

    alignas(512) uint32_t g_window[0x200u / 4u];

    uint32_t word(uintptr_t offset)
    {
        return g_window[offset / 4u];
    }

    void prefill()
    {
        for (uint32_t& w : g_window)
        {
            w = SENTINEL;
        }
    }
}

extern "C"
{
    int kos_notify_bind(kos_cap_t)
    {
        return 0;
    }

    int kos_notify_wait(kos_cap_t, uint32_t, uint32_t, uint32_t*)
    {
        return 0;
    }

    int kos_irq_ack(kos_cap_t)
    {
        return 0;
    }

    uint32_t kos_periph_clock_hz(uintptr_t)
    {
        return PERIPH_HZ;
    }

    int kos_periph_reg_write(uintptr_t base, uintptr_t offset, uint32_t value)
    {
        memcpy(reinterpret_cast<void*>(base + offset), &value, sizeof(value));
        return 0;
    }
}

bool kickos::xmc::driver::usic_tx_ready(uintptr_t)
{
    return true;
}

namespace
{
    int32_t spi_open_on(uint32_t index)
    {
        prefill();
        struct kos_spi_bus_config cfg = {};
        cfg.base = reinterpret_cast<uintptr_t>(g_window);
        cfg.ep = KOS_CAP_NONE;
        cfg.irq = 3;
        cfg.notify = 4;
        cfg.notify_bit = 0u;
        cfg.irq_index = index;
        struct kos_spi_bus bus = {};
        return kos_spi_bus_open(&bus, &cfg);
    }

    struct kos_uart_stats g_stats;

    int32_t uart_open_on(uint16_t index)
    {
        prefill();
        g_window[ru::off::FDR / 4u] = ru::FDR_DM_FRACTIONAL | 0x100u;
        g_window[ru::off::BRG / 4u] = 0u;
        struct kos_uart_config cfg = {};
        cfg.base = reinterpret_cast<uintptr_t>(g_window);
        cfg.stats = &g_stats;
        cfg.baud = 0u;
        cfg.data_bits = 8u;
        cfg.parity = KOS_UART_PARITY_NONE;
        cfg.stop_bits = 1u;
        cfg.line_index = index;
        struct kos_uart dev = {};
        return kos_uart_open(&dev, &cfg);
    }
}

TEST(UsicRoute, the_receive_events_go_to_the_service_request_the_line_index_names)
{
    for (uint32_t index = 0; index <= ru::INPR_SR_LAST; index++)
    {
        EXPECT_EQ(spi_open_on(index), 0) << index;
        EXPECT_EQ(word(ru::off::INPR), (index << ru::INPR_RINP_SHIFT) | (index << ru::INPR_AINP_SHIFT))
            << index;
    }
}

TEST(UsicRoute, a_line_index_past_the_module_s_service_requests_is_refused_before_a_store)
{
    EXPECT_EQ(spi_open_on(ru::INPR_SR_LAST + 1u), -KOS_EINVAL);
    EXPECT_EQ(word(ru::off::KSCFG), SENTINEL);
    EXPECT_EQ(word(ru::off::INPR), SENTINEL);
}

TEST(UsicRoute, the_uart_s_transmit_event_goes_to_the_service_request_the_line_index_names)
{
    for (uint16_t index = 0; index <= ru::INPR_SR_LAST; index++)
    {
        EXPECT_GT(uart_open_on(index), 0) << index;
        EXPECT_EQ(word(ru::off::INPR),
                  (SENTINEL & ~ru::INPR_TBINP_MASK) | (static_cast<uint32_t>(index) << ru::INPR_TBINP_SHIFT))
            << index;
    }
}

TEST(UsicRoute, a_uart_line_index_past_the_module_s_service_requests_is_refused_before_a_store)
{
    EXPECT_EQ(uart_open_on(static_cast<uint16_t>(ru::INPR_SR_LAST + 1u)), -KOS_EINVAL);
    EXPECT_EQ(word(ru::off::INPR), SENTINEL);
    EXPECT_EQ(word(ru::off::CCR), SENTINEL);
}
