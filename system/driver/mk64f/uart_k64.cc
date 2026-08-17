// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// K64F UART0 backend of <kickos/driver/uart.h>. Register facts: K64 Sub-Family Reference
// Manual (K64P144M120SF5RM).
//
// BYTE-mapped: a 32-bit access at base+4 spans S1/S2/C3/D and reading D pops the RX FIFO,
// so every access here is an 8-bit load/store.
//
// TDRE (S1 bit 7) resets SET and re-asserts while the TWFIFO.TXWATER condition holds
// (RM 52.3.5), so arming TIE on an idle channel raises immediately.
//
// OR/NF/FE/PF raise IRQ 32, which nothing claims; TDRE/TC/RDRF raise IRQ 31 (RM 3.2.2.3
// Table 3-5). FE inhibits further reception and OR blocks RDRF until cleared (RM 52.3.5),
// so a reader that ignores those flags leaves the receiver permanently dead.

#include <kickos/driver/uart.h>

#include <kickos/io/mmio.h>   // r8
#include <kickos/sys.h>       // kos_periph_enable, kos_periph_clock_hz
#include <kickos/sys/errno.h> // KOS_EPERM, KOS_ENOTSUP, KOS_EBUSY

#include <regs/uart.h>
#include <uart_class.h> // uart0_tx_ready

#include <stdint.h>

namespace
{
    namespace ru = kickos::mk64f::reg::uart;

    constexpr uint32_t POLL_MAX = 1000000u;

    bool tx_idle(uintptr_t base)
    {
        uint8_t const s1 = r8(base + ru::S1_OFFSET);
        return (s1 & (ru::S1_TDRE | ru::S1_TC)) == (ru::S1_TDRE | ru::S1_TC);
    }

    void tie_set(uintptr_t base, bool on)
    {
        uint8_t const c2 = r8(base + ru::C2_OFFSET);
        if (on)
        {
            r8(base + ru::C2_OFFSET) = static_cast<uint8_t>(c2 | ru::C2_TIE);
            return;
        }
        r8(base + ru::C2_OFFSET) = static_cast<uint8_t>(c2 & ~ru::C2_TIE);
    }

    // C1.M=0 is an 8-bit frame TOTAL: an enabled parity bit REPLACES the eighth data bit
    // (RM 52.4.4.1 Table 52-11, RM 52.3.8 note). 8 data bits plus parity is the 9-bit frame,
    // and 7-bit-no-parity has no encoding on this part.
    bool encode_frame(struct kos_uart_config const* cfg, uint8_t* out_c1, uint8_t* out_sbns)
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
        uint8_t c1 = 0u;
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
            c1 = static_cast<uint8_t>(c1 | ru::C1_M);
        }
        if (not expressible)
        {
            return false;
        }
        if (has_parity)
        {
            c1 = static_cast<uint8_t>(c1 | ru::C1_PE);
        }
        if (cfg->parity == KOS_UART_PARITY_ODD)
        {
            c1 = static_cast<uint8_t>(c1 | ru::C1_PT);
        }
        *out_c1 = c1;
        *out_sbns = 0u;
        if (cfg->stop_bits == 2u)
        {
            *out_sbns = ru::BDH_SBNS;
        }
        return true;
    }

    // baud = clk / (16 x (SBR + BRFA/32)) (RM 52.4.3), rearranged to clk*2 / (SBR*32 + BRFA)
    // to stay in 32-bit arithmetic. UART0 is CORE-clocked while UART2..4 are bus-clocked
    // (RM 5.7.10), so the rate must come from the per-block clock oracle.
    int32_t achieved_baud(uintptr_t base)
    {
        uint32_t const clk = kos_periph_clock_hz(base);
        if (clk == 0u)
        {
            return -KOS_ENOSYS;
        }
        uint32_t const sbr = (static_cast<uint32_t>(r8(base + ru::BDH_OFFSET) & ru::BDH_SBR_MASK)
                              << 8)
                             | r8(base + ru::BDL_OFFSET);
        uint32_t const brfa = r8(base + ru::C4_OFFSET) & ru::C4_BRFA_MASK;
        uint32_t const div32 = sbr * 32u + brfa;
        if (div32 == 0u)
        {
            return -KOS_ENOTSUP; // SBR 0 disables the generator (RM 52.3.1)
        }
        return static_cast<int32_t>((clk * 2u) / div32);
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
    uint8_t c1 = 0u;
    uint8_t sbns = 0u;
    if (not encode_frame(cfg, &c1, &sbns))
    {
        return -KOS_ENOTSUP;
    }

    // Every register access below is supervisor-only and faults until this returns: the AIPS
    // slot resets with Supervisor-Protect set (RM 3.3.8.4, PACRN reset 0x4444_4444; RM 20.2.3
    // for the SP field).
    if (kos_periph_enable(cfg->base) != 0)
    {
        return -KOS_EPERM;
    }

    u->base = cfg->base;
    u->stats = cfg->stats;

    // PFIFO is writable only while TE and RE are clear (RM 52.3.16), and a byte still in the
    // shifter would be truncated on the wire.
    for (uint32_t i = 0; i < POLL_MAX; i++)
    {
        if (tx_idle(u->base))
        {
            break;
        }
    }
    r8(u->base + ru::C2_OFFSET) = 0u;

    // A restarted driver inherits what the previous instance left; each of these breaks the
    // wire silently rather than failing.
    r8(u->base + ru::PFIFO_OFFSET) = 0u; // FIFOs off, buffers depth 1 (RM 52.3.16)
    // RM 52.3.16 requires a flush immediately after any PFIFO change; it also re-aligns a
    // receive buffer left misaligned by a previous instance (RM 52.3.5 note).
    r8(u->base + ru::CFIFO_OFFSET) =
        static_cast<uint8_t>(ru::CFIFO_TXFLUSH | ru::CFIFO_RXFLUSH);
    r8(u->base + ru::MODEM_OFFSET) = 0u; // TXCTSE: an absent CTS stalls every byte
    r8(u->base + ru::C3_OFFSET) = 0u;    // TXINV: an inverted TX corrupts every frame
    r8(u->base + ru::S2_OFFSET) = 0u;    // LBKDE: prevents RDRF from ever setting
    r8(u->base + ru::C5_OFFSET) = 0u;    // TDMAS/RDMAS: a DMA request in place of ours
    r8(u->base + ru::IR_OFFSET) = 0u;    // IREN: infrared modulation on the TX pin
    r8(u->base + ru::C7816_OFFSET) = 0u; // ISO-7816 framing

    if (cfg->baud == 0u)
    {
        // A BDH write is buffered and takes effect only when BDL is written (RM 52.3.1), so
        // the pair must be rewritten together even though only SBNS changes.
        uint8_t const bdh = r8(u->base + ru::BDH_OFFSET);
        uint8_t const bdl = r8(u->base + ru::BDL_OFFSET);
        r8(u->base + ru::BDH_OFFSET) = static_cast<uint8_t>((bdh & ru::BDH_SBR_MASK) | sbns);
        r8(u->base + ru::BDL_OFFSET) = bdl;
    }
    else
    {
        uint32_t const clk = kos_periph_clock_hz(u->base);
        if (clk == 0u)
        {
            return -KOS_ENOSYS;
        }
        uint32_t const sbr = clk / (16u * cfg->baud);
        if (sbr == 0u or sbr > 8191u)
        {
            return -KOS_ENOTSUP; // SBR 0 disables the generator (RM 52.3.1); 13 bits is the range
        }
        uint32_t const brfa = (clk * 2u) / cfg->baud - sbr * 32u;
        r8(u->base + ru::BDH_OFFSET) =
            static_cast<uint8_t>(((sbr >> 8) & ru::BDH_SBR_MASK) | sbns);
        r8(u->base + ru::BDL_OFFSET) = static_cast<uint8_t>(sbr & 0xFFu);
        r8(u->base + ru::C4_OFFSET) = static_cast<uint8_t>(brfa & ru::C4_BRFA_MASK);
    }

    r8(u->base + ru::C1_OFFSET) = c1;
    r8(u->base + ru::C2_OFFSET) = static_cast<uint8_t>(ru::C2_TE | ru::C2_RE | ru::C2_RIE);

    // The divisor registers truncate, so the achieved rate is read back, not the request.
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
        // S1 is the FIRST HALF of every clear sequence on this part (RM 52.3.5, "read the
        // status register followed by a read or write to D"), so it is re-read per iteration.
        uint8_t const s1 = r8(u->base + ru::S1_OFFSET);
        if ((s1 & ru::S1_RX_ERRORS) != 0u)
        {
            if ((s1 & ru::S1_OR) != 0u)
            {
                kos_counter_increment(&u->stats->rx_overrun, 1u);
            }
            if ((s1 & ru::S1_FE) != 0u)
            {
                kos_counter_increment(&u->stats->rx_framing, 1u);
            }
            if ((s1 & ru::S1_PF) != 0u)
            {
                kos_counter_increment(&u->stats->rx_parity, 1u);
            }
            // NF flags noise on a byte that framed correctly, so it is not folded into
            // rx_framing; it is still cleared with the others.
            //
            // Through a named copy, not a cast to void: discarding a volatile lvalue does not
            // perform the access at all, and the ACCESS is the second half of the clear
            // sequence.
            unsigned char const bad = r8(u->base + ru::D_OFFSET);
            (void)bad;
            // OR sets with the receive buffer EMPTY (RM 52.3.5), so the mandated read above
            // is a read of an empty D, which "causes the FIFO pointers to become misaligned.
            // A receive FIFO flush reinitializes the pointers" (RM 52.3.5 note). Without the
            // flush the first overrun corrupts reception permanently.
            if ((s1 & ru::S1_RDRF) == 0u)
            {
                r8(u->base + ru::CFIFO_OFFSET) = ru::CFIFO_RXFLUSH;
            }
            continue;
        }
        if ((s1 & ru::S1_RDRF) == 0u)
        {
            break;
        }
        dst[got] = r8(u->base + ru::D_OFFSET);
        got++;
        kos_counter_increment(&u->stats->rx_bytes, 1u);
    }
    return got;
}

uint32_t kos_uart_write(struct kos_uart* u, unsigned char const* src, uint32_t n)
{
    uint32_t i = 0;
    while (i < n)
    {
        if (not kickos::mk64f::driver::uart0_tx_ready(u->base))
        {
            break;
        }
        r8(u->base + ru::D_OFFSET) = src[i];
        i++;
    }
    // TIE is armed only when the device refused a byte: arming it on an idle channel storms,
    // TDRE resetting SET.
    tie_set(u->base, i < n);
    return i;
}

int32_t kos_uart_flush(struct kos_uart* u)
{
    // THE STRONG CONTRACT ON THIS PART: drained means the last stop bit has left the pin.
    // TDRE alone is only the data register handing off to the shifter; TC is set when the
    // transmitter is idle with nothing pending (RM 52.3.5).
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
    // TRUNCATES a frame still shifting, which is why the class contract puts flush before
    // close. C2 is writable at any time (RM 52.3.4).
    r8(u->base + ru::C2_OFFSET) = 0u;
    return 0;
}

}
