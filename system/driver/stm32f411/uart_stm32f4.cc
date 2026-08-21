// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// STM32F411 USART backend of <kickos/driver/uart.h>. Register facts are clean-room from
// RM0383 Rev 4 sec.19; no vendor HAL/CMSIS source.
//
// The instance base arrives in the cfg, so this one body serves any USART on the part; the
// console channel is USART2, the only one a service list grants.
//
// Clock gating belongs to the kernel: RCC_APB1ENR (RM0383 sec.6.3.11) is an
// arch_reserved_blocks entry no domain can be granted, and arch_init has already clocked the
// console channel before open() runs. APB1 carries no bus-side supervisor-protect register,
// so a granted window is live the moment it is granted.
//
// THE PIN MUX LIVES IN GPIOA, OUTSIDE THIS WINDOW: PA2/PA3 are the console channel's pins,
// and arch_pinmux_set holds both as kernel-owned.
//
// ORE (SR bit 3) is the trap on this part: a bare DR read clears RXNE but NOT ORE, and with
// RXNEIE armed a stuck ORE holds the single USART vector asserted forever. Every error latch
// needs the SR-then-DR pair (RM0383 sec.19.6.1).

#include <kickos/driver/uart.h>

#include <kickos/io/mmio.h>   // r32
#include <kickos/sys.h>       // kos_periph_clock_hz
#include <kickos/sys/errno.h> // KOS_ENOTSUP, KOS_ENOSYS, KOS_EBUSY

#include <regs/usart.h>
#include <usart_class.h> // usart_tx_ready

#include <stdint.h>

namespace
{
    namespace ru = kickos::stm32f411::reg::usart;

    constexpr uint32_t POLL_MAX = 1000000u;

    bool tx_idle(uintptr_t base)
    {
        return (r32(base + ru::SR_OFFSET) & ru::SR_TC) != 0u;
    }

    void txeie_set(uintptr_t base, bool on)
    {
        uint32_t const cr1 = r32(base + ru::CR1_OFFSET);
        if (on)
        {
            r32(base + ru::CR1_OFFSET) = cr1 | ru::CR1_TXEIE;
            return;
        }
        r32(base + ru::CR1_OFFSET) = cr1 & ~ru::CR1_TXEIE;
    }

    // M=0 is an 8-bit frame TOTAL: an enabled parity bit REPLACES the eighth data bit
    // (RM0383 sec.19.3.7 Table 85), so 8 data bits plus parity is the 9-bit frame (M=1). The
    // encodable set is 8N, 7E/7O and 8E/8O.
    bool encode_frame(struct kos_uart_config const* cfg, uint32_t* out_cr1, uint32_t* out_cr2)
    {
        if (cfg->stop_bits != 1u and cfg->stop_bits != 2u)
        {
            return false;
        }
        if (cfg->parity != KOS_UART_PARITY_NONE and cfg->parity != KOS_UART_PARITY_EVEN
            and cfg->parity != KOS_UART_PARITY_ODD)
        {
            return false;
        }
        bool const has_parity = (cfg->parity != KOS_UART_PARITY_NONE);
        uint32_t cr1 = 0u;
        bool expressible = false;
        if (cfg->data_bits == 8u and not has_parity)
        {
            expressible = true;
        }
        if (cfg->data_bits == 7u and has_parity)
        {
            expressible = true;
        }
        if (cfg->data_bits == 8u and has_parity)
        {
            expressible = true;
            cr1 = cr1 | ru::CR1_M;
        }
        if (not expressible)
        {
            return false;
        }
        if (has_parity)
        {
            cr1 = cr1 | ru::CR1_PCE;
        }
        if (cfg->parity == KOS_UART_PARITY_ODD)
        {
            cr1 = cr1 | ru::CR1_PS;
        }
        *out_cr1 = cr1;
        *out_cr2 = ru::CR2_STOP_1;
        if (cfg->stop_bits == 2u)
        {
            *out_cr2 = ru::CR2_STOP_2;
        }
        return true;
    }

    // OVER8=0, so baud = fPCLK1 / BRR with BRR the whole 16-bit mantissa+fraction word
    // (RM0383 sec.19.3.4 Equation 1, sec.19.6.3). The divisor truncates, so this reports the
    // rate the channel is really running at rather than what was asked for.
    int32_t achieved_baud(uintptr_t base)
    {
        uint32_t const clk = kos_periph_clock_hz(base);
        if (clk == 0u)
        {
            return -KOS_ENOSYS;
        }
        uint32_t const brr = r32(base + ru::BRR_OFFSET) & ru::BRR_MASK;
        if (brr < ru::BRR_MIN)
        {
            return -KOS_ENOTSUP; // USARTDIV below 1 is not a rate (RM0383 sec.19.3.4)
        }
        uint32_t const rate = clk / brr;
        if (rate == 0u)
        {
            return -KOS_ENOTSUP; // the class contract admits no rate of 0
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
    uint32_t frame_cr1 = 0u;
    uint32_t frame_cr2 = 0u;
    if (not encode_frame(cfg, &frame_cr1, &frame_cr2))
    {
        return -KOS_ENOTSUP;
    }

    u->base = cfg->base;
    u->stats = cfg->stats;

    // A divisor or frame change with a byte still in the shifter corrupts it on the wire,
    // and TC is what says the shifter is idle (RM0383 sec.19.3.2).
    bool drained = false;
    for (uint32_t i = 0; i < POLL_MAX; i++)
    {
        if (tx_idle(u->base))
        {
            drained = true;
            break;
        }
    }
    if (not drained)
    {
        // Nothing in the channel has been written yet, so this leaves it as it was found.
        return -KOS_EBUSY;
    }
    r32(u->base + ru::CR1_OFFSET) = 0u; // UE=0: channel off, every source disarmed

    // A restarted driver inherits whatever the previous instance left. Each of these breaks
    // the wire silently rather than failing, so they are written rather than assumed.
    r32(u->base + ru::CR3_OFFSET) = 0u;  // CTSE/IREN/HDSEL/SCEN/NACK/DMA all off
    r32(u->base + ru::GTPR_OFFSET) = 0u; // guard time + smartcard/IrDA prescaler
    r32(u->base + ru::CR2_OFFSET) = frame_cr2;

    if (cfg->baud != 0u)
    {
        uint32_t const clk = kos_periph_clock_hz(u->base);
        if (clk == 0u)
        {
            return -KOS_ENOSYS;
        }
        uint32_t const brr = (clk + cfg->baud / 2u) / cfg->baud; // round to nearest
        if (brr < ru::BRR_MIN or brr > ru::BRR_MASK)
        {
            return -KOS_ENOTSUP; // outside the 12-bit mantissa + 4-bit fraction reach
        }
        r32(u->base + ru::BRR_OFFSET) = brr;
    }
    // baud == 0 keeps the divisor the kernel console left.

    // RXNEIE arms ORE as well as RXNE (RM0383 sec.19.6.4), which is what makes an overrun a
    // wake this driver can clear rather than a silently dead receiver.
    r32(u->base + ru::CR1_OFFSET) =
        frame_cr1 | ru::CR1_UE | ru::CR1_TE | ru::CR1_RE | ru::CR1_RXNEIE;

    int32_t const rate = achieved_baud(u->base);
    if (rate < 0)
    {
        (void)kos_uart_close(u); // a refused open must not leave the channel enabled
    }
    return rate;
}

uint32_t kos_uart_read(struct kos_uart* u, unsigned char* dst, uint32_t n)
{
    uint32_t got = 0;
    for (uint32_t i = 0; i < n; i++)
    {
        // SR is the FIRST HALF of every clear sequence on this part (RM0383 sec.19.6.1), so
        // it is re-read per iteration.
        uint32_t const sr = r32(u->base + ru::SR_OFFSET);
        if ((sr & ru::SR_RX_ERRORS) != 0u)
        {
            if ((sr & ru::SR_ORE) != 0u)
            {
                kos_counter_increment(&u->stats->rx_overrun, 1u);
            }
            if ((sr & ru::SR_FE) != 0u)
            {
                kos_counter_increment(&u->stats->rx_framing, 1u);
            }
            if ((sr & ru::SR_PE) != 0u)
            {
                kos_counter_increment(&u->stats->rx_parity, 1u);
            }
            // NF flags noise on a byte that framed correctly, so it is not folded into
            // rx_framing; it is still cleared with the others.
            //
            // Through a named copy, not a cast to void: discarding a volatile lvalue does
            // not perform the access at all, and the ACCESS is the second half of the clear
            // sequence.
            uint32_t const late = r32(u->base + ru::DR_OFFSET);
            // AN OVERRUN HERE LOSES THE ARRIVING BYTE, NOT THE PENDING ONE, so that read is
            // a data recovery and not only a clear: "the RDR register content is not lost"
            // (RM0383 sec.19.3.3). Dropping it would discard a good byte on every overrun.
            //
            // RXNE is what says whether there is a byte to recover: it reads 0 here when the
            // last valid byte was taken between the SR and the DR access, and the read is
            // then a dummy (RM0383 sec.19.3.3). PE, FE and NF each mean the receiver could
            // not trust what it decoded, so those still drop.
            if ((sr & (ru::SR_PE | ru::SR_FE | ru::SR_NF)) == 0u and (sr & ru::SR_RXNE) != 0u)
            {
                dst[got] = static_cast<unsigned char>(late & 0xFFu);
                got++;
                kos_counter_increment(&u->stats->rx_bytes, 1u);
            }
            continue;
        }
        if ((sr & ru::SR_RXNE) == 0u)
        {
            break;
        }
        dst[got] = static_cast<unsigned char>(r32(u->base + ru::DR_OFFSET) & 0xFFu);
        got++;
        kos_counter_increment(&u->stats->rx_bytes, 1u);
    }
    return got;
}

uint32_t kos_uart_write(struct kos_uart* u, unsigned char const* src, uint32_t n)
{
    // PE ALONE among the receive latches is cleared by a read of SR followed by a read OR A
    // WRITE of DR (RM0383 sec.19.6.1), which is exactly the pair below, so a parity error
    // latched between two service passes is lost to stats.rx_parity. The counter is all that
    // is lost: reception is unaffected, PEIE stays 0, and the service route opens 8N1, where
    // PCE is 0 and PE cannot set at all. A consumer that opens WITH parity must fold the SR
    // read into this loop instead of calling usart_tx_ready. ORE, FE and NF need a DR READ to
    // clear, which a transmit is not, so they survive this loop.
    uint32_t i = 0;
    while (i < n)
    {
        if (not kickos::stm32f411::driver::usart_tx_ready(u->base))
        {
            break;
        }
        r32(u->base + ru::DR_OFFSET) = src[i];
        i++;
    }
    // TXEIE is armed only when the device refused a byte: TXE resets SET on an idle
    // transmitter, so arming it with nothing left to send storms the vector.
    txeie_set(u->base, i < n);
    return i;
}

int32_t kos_uart_flush(struct kos_uart* u)
{
    // THE STRONG CONTRACT ON THIS PART: drained means the last stop bit has left the pin.
    // TXE alone is only DR handing off to the shift register; TC is set after the stop bit
    // with TXE already set (RM0383 sec.19.3.2).
    for (uint32_t i = 0; i < POLL_MAX; i++)
    {
        if (tx_idle(u->base))
        {
            return 0;
        }
    }
    return -KOS_EBUSY;
}

int32_t kos_uart_close(struct kos_uart* u)
{
    // UE=0 takes TE, RE and every interrupt enable with it in one write.
    r32(u->base + ru::CR1_OFFSET) = 0u;
    return 0;
}

}
