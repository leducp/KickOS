// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The K64F UART backend's close and open (system/driver/mk64f/uart_k64.cc) over a host buffer
// for its window. Close: the channel is stopped before the slot is re-protected, the base is
// cleared, and a second close touches neither the channel nor the seam. Open: every refusal
// leaves the slot as it found it, shut, whether it comes before the enable or after it.

#include <kickos/driver/uart.h>
#include <kickos/sys.h>
#include <kickos/sys/errno.h>

#include <gtest/gtest.h>

#include <stdint.h>
#include <string.h>

namespace
{
    constexpr uintptr_t C2 = 0x03u;
    constexpr uint8_t C2_RUNNING = 0x2Cu; // TE | RE | RIE

    alignas(32) uint8_t g_window[0x20u];

    constexpr uintptr_t S1 = 0x04u;
    constexpr uint8_t S1_IDLE = 0xC0u; // TDRE | TC

    uint32_t g_enables = 0;
    uint32_t g_clock_hz = 0;
    uint32_t g_disables = 0;
    uintptr_t g_disabled_base = 0;
    uint8_t g_c2_at_disable = 0;

    uintptr_t window()
    {
        return reinterpret_cast<uintptr_t>(g_window);
    }

    class UartK64Close : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            memset(g_window, 0, sizeof(g_window));
            g_window[C2] = C2_RUNNING;
            g_disables = 0;
            g_disabled_base = 0;
            g_c2_at_disable = 0xFFu;
            g_enables = 0;
            g_clock_hz = 60000000u;
            memset(&uart_, 0, sizeof(uart_));
            uart_.base = window();
        }

        struct kos_uart uart_;
    };
}

extern "C"
{
    int kos_periph_enable(uintptr_t)
    {
        g_enables++;
        return 0;
    }

    int kos_periph_disable(uintptr_t base)
    {
        g_disables++;
        g_disabled_base = base;
        g_c2_at_disable = *reinterpret_cast<volatile uint8_t*>(base + C2);
        return 0;
    }

    uint32_t kos_periph_clock_hz(uintptr_t)
    {
        return g_clock_hz;
    }
}

TEST_F(UartK64Close, the_channel_stops_before_the_slot_shuts)
{
    EXPECT_EQ(kos_uart_close(&uart_), 0);
    EXPECT_EQ(g_disables, 1u);
    EXPECT_EQ(g_disabled_base, window());
    EXPECT_EQ(g_c2_at_disable, 0u) << "transmit and receive were off when the slot shut";
    EXPECT_EQ(uart_.base, 0u) << "the closed channel keeps no base";
}

TEST_F(UartK64Close, a_second_close_touches_nothing)
{
    ASSERT_EQ(kos_uart_close(&uart_), 0);
    g_window[C2] = C2_RUNNING;
    EXPECT_EQ(kos_uart_close(&uart_), 0);
    EXPECT_EQ(g_disables, 1u) << "no second disable";
    EXPECT_EQ(g_window[C2], C2_RUNNING) << "and no store into a slot that now faults";
}

namespace
{
    class UartK64Open : public UartK64Close
    {
      protected:
        int32_t open(uint32_t baud)
        {
            struct kos_uart_config cfg = {};
            cfg.base = window();
            cfg.stats = &stats_;
            cfg.baud = baud;
            cfg.data_bits = 8u;
            cfg.parity = KOS_UART_PARITY_NONE;
            cfg.stop_bits = 1u;
            memset(&uart_, 0, sizeof(uart_));
            return kos_uart_open(&uart_, &cfg);
        }

        // The slot is shut exactly when every enable it took was undone.
        void expect_slot_shut()
        {
            EXPECT_EQ(g_disables, g_enables) << "a refused open left the slot open";
            EXPECT_EQ(uart_.base, 0u) << "a refused open keeps no base";
        }

        struct kos_uart_stats stats_ = {};
    };
}

TEST_F(UartK64Open, a_channel_that_never_drains_is_refused_with_its_slot_shut)
{
    g_window[S1] = 0u;
    EXPECT_EQ(open(115200u), -KOS_EBUSY);
    expect_slot_shut();
    EXPECT_EQ(g_window[C2], C2_RUNNING) << "the channel is left as it was found";
}

TEST_F(UartK64Open, an_unknown_clock_is_refused_with_its_slot_shut)
{
    g_window[S1] = S1_IDLE;
    g_clock_hz = 0u;
    EXPECT_EQ(open(115200u), -KOS_ENOSYS);
    expect_slot_shut();
}

TEST_F(UartK64Open, a_rate_the_divisor_cannot_reach_is_refused_with_its_slot_shut)
{
    g_window[S1] = S1_IDLE;
    EXPECT_EQ(open(1u), -KOS_ENOTSUP) << "60 MHz / (16 x 1) is past SBR's 13 bits";
    expect_slot_shut();
}

TEST_F(UartK64Open, an_accepted_open_keeps_its_slot_open)
{
    g_window[S1] = S1_IDLE;
    EXPECT_GT(open(115200u), 0);
    EXPECT_EQ(g_enables, 1u);
    EXPECT_EQ(g_disables, 0u);
    EXPECT_EQ(uart_.base, window());
}
