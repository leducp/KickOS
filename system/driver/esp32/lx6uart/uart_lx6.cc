// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32 (Xtensa LX6) UART0 open, for a channel the ROM has already brought up. Everything
// below open is the family body in system/driver/espuart. Register facts: ESP32 TRM v5.8
// ch.19, via arch/xtensa/chip/esp32/regs/uart.h.

#include <kickos/driver/uart.h>

#include <kickos/io/mmio.h>   // r32
#include <kickos/sys.h>       // kos_periph_clock_hz
#include <kickos/sys/errno.h> // KOS_ENOTSUP, KOS_ENOSYS

#include "../../espuart/uart_esp.h"

#include <stdint.h>

namespace
{
    namespace ru = kickos::espuart::reg::uart;

    // CLKDIV is a 20-bit integer plus a 4-bit 1/16 fraction and baud = fclk / (int + frac/16)
    // (TRM Register 19.6), computed as fclk * 16 / clkdiv16 to stay in integer arithmetic.
    // Truncating, so the answer is the achieved rate rounded DOWN.
    int32_t achieved_baud(uintptr_t base)
    {
        // CLKDIV counts the APB clock only while CONF0.TICK_REF_ALWAYS_ON selects it; with
        // that bit clear the same divisor is counted against REF_TICK.
        if ((r32(base + ru::OFF_CONF0) & ru::CONF0_TICK_REF_ALWAYS_ON) == 0u)
        {
            return -KOS_ENOTSUP;
        }
        // APB is a function of a clock-source select the bootloader can have changed (TRM
        // v5.8 Table 7.2-4 p.169), and RTC_CNTL is a reserved block this window's holder
        // cannot read, so the rate comes from the kernel at runtime.
        uint32_t const clk = kos_periph_clock_hz(base);
        if (clk == 0u)
        {
            return -KOS_ENOSYS;
        }
        uint32_t const clkdiv = r32(base + ru::OFF_CLKDIV);
        uint64_t const clkdiv16 =
            (static_cast<uint64_t>(clkdiv & ru::CLKDIV_INT_MASK) << 4)
            | ((clkdiv >> ru::CLKDIV_FRAC_S) & 0xFu);
        if (clkdiv16 == 0u)
        {
            return -KOS_ENOTSUP; // a zero divisor stops the generator
        }
        uint64_t const rate = (static_cast<uint64_t>(clk) << 4) / clkdiv16;
        if (rate == 0u)
        {
            return -KOS_ENOTSUP;
        }
        return static_cast<int32_t>(rate);
    }
}

extern "C"
{

int32_t kos_uart_open(struct kos_uart* u, struct kos_uart_config const* cfg)
{
    int32_t const bad_cfg = kos_uart_cfg_check(cfg);
    if (bad_cfg != 0)
    {
        return bad_cfg;
    }
    // CLKDIV is writable, but the FIFO count reaches zero one frame before the byte leaves
    // the shifter, so a rewrite here re-times a byte still in flight.
    int32_t const fixed_rate = kos_uart_cfg_check_fixed_rate(cfg);
    if (fixed_rate != 0)
    {
        return fixed_rate;
    }
    // The frame stays the 8N1 the ROM left in CONF0.
    if (cfg->data_bits != 8u or cfg->parity != KOS_UART_PARITY_NONE or cfg->stop_bits != 1u)
    {
        return -KOS_ENOTSUP;
    }

    u->base = cfg->base;
    u->stats = cfg->stats;

    r32(u->base + ru::OFF_INT_ENA) = 0;
    r32(u->base + ru::OFF_INT_CLR) = 0xFFFFFFFFu;
    r32(u->base + ru::OFF_CONF1) =
        ((ru::TXFIFO_EMPTY_THRHD & ru::TXFIFO_EMPTY_THRHD_MASK) << ru::TXFIFO_EMPTY_THRHD_S)
        | ((ru::RXFIFO_FULL_THRHD & ru::RXFIFO_FULL_THRHD_MASK) << ru::RXFIFO_FULL_THRHD_S);
    r32(u->base + ru::OFF_INT_ENA) = kickos::espuart::RX_INT_MASK;

    int32_t const rate = achieved_baud(u->base);
    if (rate < 0)
    {
        (void)kos_uart_close(u); // a refused open must not leave a source armed
    }
    return rate;
}

}
