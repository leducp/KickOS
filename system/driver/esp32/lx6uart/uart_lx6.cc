// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32 (Xtensa LX6) UART0 backend of <kickos/driver/uart.h>, for a channel the ROM has
// already brought up. Register facts: ESP32 TRM v5.8 ch.19, via arch/xtensa/chip/esp32/regs/uart.h.
//
// UART_INT_ST == UART_INT_RAW & UART_INT_ENA, and RAW is a LATCH that only an INT_CLR write
// drops (TRM appendix "Interrupt Configuration Registers"), so every serviced source needs a
// clear.
//
// UART_TXFIFO_EMPTY's condition is level on occupancy (TRM Register 19.10), so enabling it on
// an idle channel raises immediately.
//
// UART_RXFIFO_FULL_INT_CLR "can be set only when data in Rx_FIFO is less than
// UART_RXFIFO_FULL_THRHD" (TRM Register 19.5), so with the threshold at 1 the clear must
// follow a full drain.

#include <kickos/driver/uart.h>

#include <kickos/io/mmio.h>   // r32
#include <kickos/sys.h>       // kos_periph_clock_hz
#include <kickos/sys/errno.h> // KOS_ENOTSUP, KOS_ENOSYS, KOS_EBUSY

#include <regs/uart.h>

#include <stdint.h>

namespace
{
    namespace ru = kickos::esp32::reg::uart;

    // TX-empty is armed on demand by kos_uart_write, never here.
    constexpr uint32_t RX_INT_MASK =
        ru::RXFIFO_FULL_INT | ru::RXFIFO_OVF_INT | ru::FRM_ERR_INT | ru::PARITY_ERR_INT;

    constexpr uint32_t FLUSH_POLL_MAX = 1000000u;

    uint32_t txfifo_cnt(uintptr_t base)
    {
        return (r32(base + ru::OFF_STATUS) >> ru::TXFIFO_CNT_SHIFT) & ru::TXFIFO_CNT_MASK;
    }

    uint32_t rxfifo_cnt(uintptr_t base)
    {
        return (r32(base + ru::OFF_STATUS) >> ru::RXFIFO_CNT_SHIFT) & ru::RXFIFO_CNT_MASK;
    }

    void tx_int_set(uintptr_t base, bool on)
    {
        uint32_t const ena = r32(base + ru::OFF_INT_ENA);
        if (on)
        {
            // Enable only: dropping the stale latch here would kill the re-raise when the
            // burst stopped on a full FIFO.
            r32(base + ru::OFF_INT_ENA) = ena | ru::TXFIFO_EMPTY_INT;
            return;
        }
        r32(base + ru::OFF_INT_ENA) = ena & ~ru::TXFIFO_EMPTY_INT;
    }

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
            | ((clkdiv >> ru::CLKDIV_FRAC_SHIFT) & 0xFu);
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
    // A rate request is refused by the handover, not by the silicon: CLKDIV is writable, but
    // rewriting it re-times a byte still in flight and this backend cannot tell that byte has
    // left, the FIFO count going to zero one frame early.
    if (cfg->baud != 0u)
    {
        return -KOS_ENOTSUP;
    }
    // CONF0 is not rewritten either, so the frame stays the 8N1 the ROM runs.
    if (cfg->data_bits != 8u or cfg->parity != KOS_UART_PARITY_NONE or cfg->stop_bits != 1u)
    {
        return -KOS_ENOTSUP;
    }

    u->base = cfg->base;
    u->stats = cfg->stats;

    r32(u->base + ru::OFF_INT_ENA) = 0;
    r32(u->base + ru::OFF_INT_CLR) = 0xFFFFFFFFu;
    r32(u->base + ru::OFF_CONF1) =
        ((ru::TXFIFO_EMPTY_THRHD & ru::TXFIFO_EMPTY_THRHD_MASK) << ru::TXFIFO_EMPTY_THRHD_SHIFT)
        | ((ru::RXFIFO_FULL_THRHD & ru::RXFIFO_FULL_THRHD_MASK) << ru::RXFIFO_FULL_THRHD_SHIFT);
    r32(u->base + ru::OFF_INT_ENA) = RX_INT_MASK;

    int32_t const rate = achieved_baud(u->base);
    if (rate < 0)
    {
        (void)kos_uart_close(u); // a refused open must not leave a source armed
    }
    return rate;
}

uint32_t kos_uart_read(struct kos_uart* u, unsigned char* dst, uint32_t n)
{
    // UART_FIFO_REG carries the data byte only, with no per-byte error tag: an error flag is
    // counted, and the erroneous byte itself stays in the stream. These three latches are
    // ungated, so unlike the RX-full clear below they take immediately.
    uint32_t const st = r32(u->base + ru::OFF_INT_ST);
    uint32_t err_clr = 0;
    if ((st & ru::RXFIFO_OVF_INT) != 0u)
    {
        u->stats->rx_overrun++;
        err_clr |= ru::RXFIFO_OVF_INT;
    }
    if ((st & ru::FRM_ERR_INT) != 0u)
    {
        u->stats->rx_framing++;
        err_clr |= ru::FRM_ERR_INT;
    }
    if ((st & ru::PARITY_ERR_INT) != 0u)
    {
        u->stats->rx_parity++;
        err_clr |= ru::PARITY_ERR_INT;
    }
    if (err_clr != 0u)
    {
        r32(u->base + ru::OFF_INT_CLR) = err_clr;
    }

    // One bounded pass: a byte arriving mid-pass leaves the threshold condition true, so the
    // clear below is refused and the line re-posts.
    uint32_t cnt = rxfifo_cnt(u->base);
    if (cnt > n)
    {
        cnt = n;
    }
    for (uint32_t i = 0; i < cnt; i++)
    {
        dst[i] = static_cast<unsigned char>(r32(u->base + ru::OFF_FIFO) & 0xFFu);
    }
    u->stats->rx_bytes += cnt;
    // AFTER the drain, and only then does the hardware accept it (Register 19.5).
    r32(u->base + ru::OFF_INT_CLR) = ru::RXFIFO_FULL_INT;
    return cnt;
}

uint32_t kos_uart_write(struct kos_uart* u, unsigned char const* src, uint32_t n)
{
    uint32_t i = 0;
    while (i < n)
    {
        if (txfifo_cnt(u->base) >= ru::TXFIFO_LIMIT)
        {
            break;
        }
        r32(u->base + ru::OFF_FIFO) = src[i];
        // The latch survives the FIFO passing the threshold, so it is dropped per push
        // rather than once at the end of the burst.
        r32(u->base + ru::OFF_INT_CLR) = ru::TXFIFO_EMPTY_INT;
        i++;
    }
    // Armed only when the FIFO refused a byte: arming with nothing left to send is a storm,
    // the condition being level on occupancy.
    tx_int_set(u->base, i < n);
    return i;
}

int32_t kos_uart_flush(struct kos_uart* u)
{
    // THE WEAK CONTRACT ON THIS PART: drained means the FIFO emptied, and a byte may still be
    // in the shifter. A consumer that must not clip the final byte needs a delay of its own.
    for (uint32_t i = 0; i < FLUSH_POLL_MAX; i++)
    {
        if (txfifo_cnt(u->base) == 0u)
        {
            return 0;
        }
    }
    return -KOS_EBUSY;
}

int32_t kos_uart_close(struct kos_uart* u)
{
    // CONF0 is left alone: the ROM owns that framing, and rewriting it would truncate a frame
    // still shifting.
    r32(u->base + ru::OFF_INT_ENA) = 0;
    r32(u->base + ru::OFF_INT_CLR) = 0xFFFFFFFFu;
    return 0;
}

}
