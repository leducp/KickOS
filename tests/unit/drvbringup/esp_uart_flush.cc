// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The ESP UART family's kos_uart_flush over a host buffer for its window, built once per chip:
// drained is an empty TX FIFO AND an idle transmitter state machine.

#include <kickos/driver/uart.h>

#include <kickos/sys/errno.h>

#include <regs/uart.h>

#include <stdint.h>
#include <string.h>

#include <gtest/gtest.h>

namespace
{
    namespace ru = kickos::espuart::reg::uart;

    alignas(4) uint32_t g_window[64];

    struct kos_uart window_uart()
    {
        memset(g_window, 0, sizeof(g_window));
        struct kos_uart u = {};
        u.base = reinterpret_cast<uintptr_t>(g_window);
        return u;
    }

    void set_bits(uintptr_t off, uint32_t bits)
    {
        g_window[off / sizeof(uint32_t)] |= bits;
    }

    // Any state but idle: 11 is TX_STP1 in ESP32 TRM Register 19.8.
    constexpr uint32_t TX_SHIFTING = 11u;
}

TEST(EspUartFlush, an_empty_fifo_and_an_idle_transmitter_are_drained)
{
    struct kos_uart u = window_uart();
    EXPECT_EQ(kos_uart_flush(&u), 0);
}

TEST(EspUartFlush, a_frame_still_shifting_out_of_an_empty_fifo_is_not_drained)
{
    struct kos_uart u = window_uart();
    set_bits(ru::OFF_TX_FSM, TX_SHIFTING << ru::ST_UTX_OUT_S);
    EXPECT_EQ(kos_uart_flush(&u), -KOS_EBUSY) << "the last frame is still on the pin";
}

TEST(EspUartFlush, a_queued_byte_is_not_drained)
{
    struct kos_uart u = window_uart();
    set_bits(ru::OFF_STATUS, 1u << ru::TXFIFO_CNT_S);
    EXPECT_EQ(kos_uart_flush(&u), -KOS_EBUSY);
}
