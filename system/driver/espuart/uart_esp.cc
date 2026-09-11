// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The ESP UART0 family body: read, write, flush and close, shared by the ESP32 (Xtensa LX6)
// and ESP32-C6 (RV32) backends of <kickos/driver/uart.h>. Which chip's registers these are
// is decided by the include path; see uart_esp.h.
//
// UART_INT_ST == UART_INT_RAW & UART_INT_ENA, and RAW is a LATCH that only an INT_CLR write
// drops, so every serviced source needs a clear. (ESP32 TRM v5.8 appendix "Interrupt
// Configuration Registers"; ESP32-C6 TRM v1.2 Registers 27.3 - 27.6.)
//
// UART_TXFIFO_EMPTY's condition is level on occupancy (ESP32 TRM Register 19.10; C6 TRM
// section 27.4.11), so enabling it on an idle channel raises immediately.
//
// The RXFIFO_FULL clear is refused while the FIFO still holds RXFIFO_FULL_THRHD bytes
// (ESP32 TRM Register 19.5), so it must FOLLOW the drain on both parts.

#include <kickos/driver/uart.h>

#include <kickos/io/mmio.h>   // r32
#include <kickos/sys/errno.h> // KOS_EBUSY

#include "uart_esp.h"

#include <stdint.h>

namespace
{
    namespace ru = kickos::espuart::reg::uart;

    // 8 bits is the WHOLE count only while each FIFO keeps its default 128-byte block. On
    // the ESP32 the three high bits of an extended FIFO's count live in a second register
    // (TRM Register 19.8); the C6's count is 8 bits outright (TRM Register 27.21).
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
            // Enable only: the stale latch IS the re-raise for a burst that stopped on a
            // full FIFO.
            r32(base + ru::OFF_INT_ENA) = ena | ru::TXFIFO_EMPTY_INT;
            return;
        }
        r32(base + ru::OFF_INT_ENA) = ena & ~ru::TXFIFO_EMPTY_INT;
    }
}

extern "C"
{

uint32_t kos_uart_read(struct kos_uart* u, unsigned char* dst, uint32_t n)
{
    // UART_FIFO_REG carries the data byte only, with no per-byte error tag: an error flag is
    // counted, and the erroneous byte itself stays in the stream. These three latches are
    // ungated, so the clear takes immediately.
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
    kos_counter_increment(&u->stats->rx_bytes, cnt);
    // AFTER the drain: refused while the threshold still holds.
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
        // The latch survives the FIFO passing the threshold, so it is dropped per push.
        r32(u->base + ru::OFF_INT_CLR) = ru::TXFIFO_EMPTY_INT;
        i++;
    }
    // The condition is level on occupancy, so arming with nothing left to send is a storm.
    tx_int_set(u->base, i < n);
    return i;
}

int32_t kos_uart_flush(struct kos_uart* u)
{
    // THE WEAK CONTRACT ON THIS FAMILY: drained means the FIFO emptied, and a byte may still
    // be in the shifter. Neither TRM documents a shifter-empty indication, so a consumer that
    // must not clip the final byte needs a delay of its own.
    for (uint32_t i = 0; i < kickos::espuart::POLL_MAX; i++)
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
    // Framing and enable belong to the ROM on the LX6 and to PCR on the C6, and a CONF0
    // rewrite truncates a frame still shifting.
    r32(u->base + ru::OFF_INT_ENA) = 0;
    r32(u->base + ru::OFF_INT_CLR) = 0xFFFFFFFFu;
    return 0;
}

}
