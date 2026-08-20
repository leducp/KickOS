// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 UART0 backend of <kickos/driver/uart.h>, for a channel the ROM has already brought
// up. Register facts: ESP32-C6 TRM v1.2 ch.27, via arch/riscv/chip/esp32c6/regs/uart.h.
//
// UART_INT_ST == UART_INT_RAW & UART_INT_ENA, and RAW is R/WTC/SS: a LATCH that only an
// INT_CLR write drops (TRM Registers 27.3 - 27.6), so every serviced source needs a clear.
//
// UART_TXFIFO_EMPTY's condition is level on occupancy (TRM section 27.4.11), so enabling it
// on an idle channel raises immediately.
//
// A write to a _SYNC register is inert until UART_REG_UPDATE carries it across (TRM section
// 27.5.1). CLKDIV_SYNC and CONF0_SYNC are both _SYNC; CONF1 is not.

#include <kickos/driver/uart.h>

#include <kickos/io/mmio.h>   // r32
#include <kickos/sys.h>       // kos_periph_clock_hz
#include <kickos/sys/errno.h> // KOS_ENOTSUP, KOS_ENOSYS, KOS_EBUSY

#include <regs/uart.h>

#include <stdint.h>

namespace
{
    namespace ru = kickos::esp32c6::reg::uart;

    // Threshold 1: every byte raises. 32 leaves the transmitter a quarter of its 128-byte
    // FIFO to run on before the refill wake arrives.
    constexpr uint32_t RX_FULL_THRHD = 1;
    constexpr uint32_t TX_EMPTY_THRHD = 32;

    // TX-empty is armed on demand by kos_uart_write, never here.
    constexpr uint32_t RX_INT_MASK =
        ru::RXFIFO_FULL_INT | ru::RXFIFO_OVF_INT | ru::FRM_ERR_INT | ru::PARITY_ERR_INT;

    constexpr uint32_t POLL_MAX = 1000000u;

    uint32_t txfifo_cnt(uintptr_t base)
    {
        return (r32(base + ru::OFF_STATUS) >> ru::TXFIFO_CNT_S) & ru::TXFIFO_CNT_MASK;
    }

    uint32_t rxfifo_cnt(uintptr_t base)
    {
        return (r32(base + ru::OFF_STATUS) >> ru::RXFIFO_CNT_S) & ru::RXFIFO_CNT_MASK;
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

    // Reading UART_REG_UPDATE back as 0 is what proves the LAST crossing finished (TRM
    // section 27.5.2.2), so it is waited for on both sides of a _SYNC burst.
    bool sync_idle(uintptr_t base)
    {
        for (uint32_t i = 0; i < POLL_MAX; i++)
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

    // RX_INT_MASK is armed only BELOW this, which is what lets every refusal above return
    // bare where the other backends call kos_uart_close: INT_ENA is still 0, and close on
    // this part clears no enable bit because the part has none.
    int32_t const rate = achieved_baud(u->base);
    if (rate < 0)
    {
        return rate;
    }

    r32(u->base + ru::OFF_INT_ENA) = RX_INT_MASK;
    return rate;
}

uint32_t kos_uart_read(struct kos_uart* u, unsigned char* dst, uint32_t n)
{
    // UART_FIFO_REG carries the data byte only, with no per-byte error tag: an error flag is
    // counted, and the erroneous byte itself stays in the stream.
    uint32_t const st = r32(u->base + ru::OFF_INT_ST);
    uint32_t err_clr = 0;
    if ((st & ru::RXFIFO_OVF_INT) != 0u)
    {
        kos_counter_increment(&u->stats->rx_overrun, 1u);
        err_clr |= ru::RXFIFO_OVF_INT;
    }
    if ((st & ru::FRM_ERR_INT) != 0u)
    {
        kos_counter_increment(&u->stats->rx_framing, 1u);
        err_clr |= ru::FRM_ERR_INT;
    }
    if ((st & ru::PARITY_ERR_INT) != 0u)
    {
        kos_counter_increment(&u->stats->rx_parity, 1u);
        err_clr |= ru::PARITY_ERR_INT;
    }
    if (err_clr != 0u)
    {
        r32(u->base + ru::OFF_INT_CLR) = err_clr;
    }

    uint32_t cnt = rxfifo_cnt(u->base);
    if (cnt > n)
    {
        cnt = n;
    }
    for (uint32_t i = 0; i < cnt; i++)
    {
        dst[i] = static_cast<unsigned char>(r32(u->base + ru::OFF_FIFO) & 0xFFu);
    }
    kos_counter_increment(&u->stats->rx_bytes, cnt);
    // AFTER the drain: clearing first would drop the notification for a byte landing
    // mid-pass, the condition being "data in RX FIFO greater than or equal to
    // UART_RXFIFO_FULL_THRHD" (TRM section 27.4.2).
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
    // in the shifter. TRM v1.2 documents no shifter-empty indication, so a consumer that must
    // not clip the final byte needs a delay of its own.
    for (uint32_t i = 0; i < POLL_MAX; i++)
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
    // CONF0_SYNC is left alone: this part has no transmit or receive enable to clear, and
    // gating the channel's clock means PCR, which belongs to the kernel.
    r32(u->base + ru::OFF_INT_ENA) = 0;
    r32(u->base + ru::OFF_INT_CLR) = 0xFFFFFFFFu;
    return 0;
}

}
