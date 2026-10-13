// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The kernel console's edge on the ESP UART, for a part that aliases its register namespace into
// kickos::espuart (its regs/uart.h, included first): the ring's TX backend and the polled writer.

#ifndef KICKOS_ARCH_COMMON_ESP_UART_CONSOLE_H
#define KICKOS_ARCH_COMMON_ESP_UART_CONSOLE_H

#include <kickos/arch/console_poll.h>
#include <kickos/console_tx.h>
#include <kickos/io/mmio.h>

#include <stddef.h>
#include <stdint.h>

namespace kickos::espuart::console
{
    inline int tx_slot_free(void)
    {
        uint32_t const queued = (r32(reg::uart::STATUS) >> reg::uart::TXFIFO_CNT_S)
                                & reg::uart::TXFIFO_CNT_MASK;
        if (queued < reg::uart::TXFIFO_LIMIT)
        {
            return 1;
        }
        return 0;
    }

    // The TX-empty source is a latch only INT_CLR drops, and the kernel drain runs with its line
    // unmasked: left standing once the FIFO passed the threshold, it re-enters the dispatcher for
    // as long as the ring holds bytes.
    inline void tx_push(uint8_t b)
    {
        r32(reg::uart::FIFO) = b;
        r32(reg::uart::INT_CLR) = reg::uart::TXFIFO_EMPTY_INT;
    }

    // The stale latch of a stopped burst is dropped first, or it raises the instant the source
    // is armed.
    inline void tx_irq_enable(void)
    {
        r32(reg::uart::INT_CLR) = reg::uart::TXFIFO_EMPTY_INT;
        r32(reg::uart::INT_ENA) = r32(reg::uart::INT_ENA) | reg::uart::TXFIFO_EMPTY_INT;
    }

    inline void tx_irq_disable(void)
    {
        r32(reg::uart::INT_ENA) = r32(reg::uart::INT_ENA) & ~reg::uart::TXFIFO_EMPTY_INT;
    }

    inline constexpr console_tx_backend backend = {tx_slot_free, tx_push, tx_irq_enable,
                                                   tx_irq_disable, &kickos_console_tx_storage};

    inline bool write_sync(char const* buf, size_t n)
    {
        return console_poll::write<tx_slot_free, tx_push>(buf, n);
    }
}

#endif
