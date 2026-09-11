// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 UART0 open, for a channel the ROM has already brought up. Everything below open
// is the family body in system/driver/espuart. Register facts: ESP32-C6 TRM v1.2 ch.27, via
// arch/riscv/chip/esp32c6/regs/uart.h.
//
// A write to a _SYNC register is inert until UART_REG_UPDATE carries it across (TRM section
// 27.5.1). CLKDIV_SYNC and CONF0_SYNC are both _SYNC; CONF1 is not.

#include <kickos/driver/uart.h>

#include <kickos/io/mmio.h>   // r32
#include <kickos/sys.h>       // kos_periph_clock_hz
#include <kickos/sys/errno.h> // KOS_ENOTSUP, KOS_ENOSYS, KOS_EBUSY

#include "../../espuart/uart_esp.h"

#include <stdint.h>

namespace
{
    namespace ru = kickos::espuart::reg::uart;

    // Threshold 1: every byte raises. 32 leaves the transmitter a quarter of its 128-byte
    // FIFO to run on before the refill wake arrives.
    constexpr uint32_t RX_FULL_THRHD = 1;
    constexpr uint32_t TX_EMPTY_THRHD = 32;

    // Reading UART_REG_UPDATE back as 0 is what proves the LAST crossing finished (TRM
    // section 27.5.2.2), so it is waited for on both sides of a _SYNC burst.
    bool sync_idle(uintptr_t base)
    {
        for (uint32_t i = 0; i < kickos::espuart::POLL_MAX; i++)
        {
            if ((r32(base + ru::OFF_REG_UPDATE) & ru::REG_UPDATE_BIT) == 0u)
            {
                return true;
            }
        }
        return false;
    }

    bool sync_commit(uintptr_t base)
    {
        r32(base + ru::OFF_REG_UPDATE) = ru::REG_UPDATE_BIT;
        return sync_idle(base);
    }

    // Parity here is an EXTRA bit rather than a replacement for the eighth data bit, so
    // BIT_NUM counts data bits alone (TRM Register 27.9).
    bool encode_frame(struct kos_uart_config const* cfg, uint32_t* out_conf0)
    {
        if (cfg->data_bits < 5u or cfg->data_bits > 8u)
        {
            return false;
        }
        if (cfg->stop_bits != 1u and cfg->stop_bits != 2u)
        {
            return false;
        }
        uint32_t conf0 = static_cast<uint32_t>(cfg->data_bits - 5u) << ru::CONF0_BIT_NUM_S;
        if (cfg->stop_bits == 1u)
        {
            conf0 |= 1u << ru::CONF0_STOP_BIT_NUM_S;
        }
        else
        {
            conf0 |= 3u << ru::CONF0_STOP_BIT_NUM_S;
        }
        if (cfg->parity == KOS_UART_PARITY_EVEN)
        {
            conf0 |= ru::CONF0_PARITY_EN;
        }
        else if (cfg->parity == KOS_UART_PARITY_ODD)
        {
            conf0 |= ru::CONF0_PARITY_EN | ru::CONF0_PARITY;
        }
        else if (cfg->parity != KOS_UART_PARITY_NONE)
        {
            return false;
        }
        *out_conf0 = conf0;
        return true;
    }

    // CLKDIV_SYNC is a 12-bit integer plus a 4-bit 1/16 fraction and baud = fclk /
    // (int + frag/16) (TRM section 27.4.3.1), computed as fclk * 16 / clkdiv16 to stay in
    // integer arithmetic. Truncating, so the answer is the achieved rate rounded DOWN.
    int32_t achieved_baud(uintptr_t base)
    {
        // PCR selects and divides UART0's source clock and is a kernel-reserved block, so the
        // holder of this window cannot read the select itself.
        uint32_t const clk = kos_periph_clock_hz(base);
        if (clk == 0u)
        {
            return -KOS_ENOSYS;
        }
        uint32_t const d = r32(base + ru::OFF_CLKDIV_SYNC);
        uint32_t const clkdiv16 = ((d & ru::CLKDIV_INT_MASK) << 4)
                                  | ((d >> ru::CLKDIV_FRAG_S) & ru::CLKDIV_FRAG_MASK);
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
    uint32_t frame = 0;
    if (not encode_frame(cfg, &frame))
    {
        return -KOS_ENOTSUP;
    }

    u->base = cfg->base;
    u->stats = cfg->stats;

    r32(u->base + ru::OFF_INT_ENA) = 0;
    r32(u->base + ru::OFF_INT_CLR) = 0xFFFFFFFFu;

    // Read-modify-write: the rest of CONF1 carries the pad-inversion and flow-control bits
    // the ROM left.
    uint32_t conf1 = r32(u->base + ru::OFF_CONF1);
    conf1 &= ~(ru::RXFIFO_FULL_THRHD_MASK << ru::RXFIFO_FULL_THRHD_S);
    conf1 &= ~(ru::TXFIFO_EMPTY_THRHD_MASK << ru::TXFIFO_EMPTY_THRHD_S);
    conf1 |= (RX_FULL_THRHD & ru::RXFIFO_FULL_THRHD_MASK) << ru::RXFIFO_FULL_THRHD_S;
    conf1 |= (TX_EMPTY_THRHD & ru::TXFIFO_EMPTY_THRHD_MASK) << ru::TXFIFO_EMPTY_THRHD_S;
    r32(u->base + ru::OFF_CONF1) = conf1;

    // The divisor and the frame move only on a drained transmitter: a byte still queued would
    // be re-timed or re-framed on the wire.
    if (kos_uart_flush(u) != 0)
    {
        return -KOS_EBUSY;
    }
    if (not sync_idle(u->base))
    {
        return -KOS_EBUSY;
    }

    if (cfg->baud != 0u)
    {
        uint32_t const clk = kos_periph_clock_hz(u->base);
        if (clk == 0u)
        {
            return -KOS_ENOSYS;
        }
        uint32_t const clkdiv16 =
            static_cast<uint32_t>((static_cast<uint64_t>(clk) << 4) / cfg->baud);
        uint32_t const integral = clkdiv16 >> 4;
        if (integral == 0u or integral > ru::CLKDIV_INT_MASK)
        {
            // 12 bits of integer divisor, and PCR's own divider is reserved to the kernel.
            return -KOS_ENOTSUP;
        }
        r32(u->base + ru::OFF_CLKDIV_SYNC) =
            integral | ((clkdiv16 & ru::CLKDIV_FRAG_MASK) << ru::CLKDIV_FRAG_S);
    }

    // Only the four framing fields: the rest of CONF0_SYNC carries UART_MEM_CLK_EN, which
    // resets SET and gates the FIFO RAM clock.
    uint32_t conf0 = r32(u->base + ru::OFF_CONF0_SYNC);
    conf0 &= ~(ru::CONF0_PARITY | ru::CONF0_PARITY_EN
               | (ru::CONF0_BIT_NUM_MASK << ru::CONF0_BIT_NUM_S)
               | (ru::CONF0_STOP_BIT_NUM_MASK << ru::CONF0_STOP_BIT_NUM_S));
    conf0 |= frame;
    r32(u->base + ru::OFF_CONF0_SYNC) = conf0;

    if (not sync_commit(u->base))
    {
        return -KOS_EBUSY;
    }

    // RX_INT_MASK is armed only BELOW this, so every refusal above returns with INT_ENA
    // still 0 and the channel quiet.
    int32_t const rate = achieved_baud(u->base);
    if (rate < 0)
    {
        return rate;
    }

    r32(u->base + ru::OFF_INT_ENA) = kickos::espuart::RX_INT_MASK;
    return rate;
}

}
