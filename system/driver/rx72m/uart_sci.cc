// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The RX72M SCI backend of the raw UART class <kickos/driver/uart.h>, for a channel the
// kernel has already clocked and pinned (sci6_console_init) and released at the console
// handover.
//
// Every function here touches the granted register window, so all of them run in the single
// thread holding that grant. The register file is BYTE-mapped.
//
// Register facts come from the RX72M Group User's Manual: Hardware (r01uh0804ej0120,
// Rev.1.20) sec.42, via arch/rx/chip/rx72m/regs/sci.h. Four shape the whole file:
//
//   1. TXI is raised only by a TDR -> TSR transfer taken with TIE ALREADY 1
//      (sec.42.12.2(1) p.2308), so kos_uart_write arms TIE BEFORE it writes TDR.
//   2. Reception is STOPPED while any of ORER/FER/PER is 1 (sec.42.3.9 p.2241), and
//      recovering from an overrun requires READING RDR (Fig.42.21 step [6] p.2243), so
//      kos_uart_read must be called even by a consumer that discards the bytes.
//   3. Clearing a status flag is NOT a read-modify-write: write 0 to a bit that read 1
//      (Note 1 p.2171), and 1 into the TDRE and RDRF positions (Note 2).
//   4. SMR, SCMR, SEMR and BRR are writable ONLY with SCR.TE and SCR.RE both 0 (notes on
//      sec.42.2.9/12/13/15), so open takes the channel down first.
//
// SCR.TEIE stays 0 and RIE covers ERI: both are GROUPBL0 LEVEL sources, and the thread that
// relays this driver's second line owns no register, so it cannot clear the peripheral flag
// that makes the level drop.

#include <kickos/driver/uart.h>

#include <kickos/io/mmio.h>   // r8
#include <kickos/sys.h>       // kos_periph_clock_hz
#include <kickos/sys/errno.h> // KOS_ENOTSUP, KOS_ENOSYS, KOS_EBUSY

#include <regs/sci.h> // the chip's shared SCI register map, not a local copy

#include <stdint.h>

namespace
{
    namespace rs = kickos::rx::reg::sci;

    // Per-poll cap: a channel that never reports idle costs a bounded delay, not the
    // calling thread. One 8N1 frame at 115200 baud is ~87 us.
    constexpr uint32_t POLL_MAX = 1000000u;

    bool tx_idle(uintptr_t base)
    {
        // TEND, not TDRE: TDRE says the holding register is free, TEND says the shift
        // register has finished and nothing is queued behind it (sec.42.2.11 p.2171).
        return (r8(base + rs::SSR_OFFSET) & rs::SSR_TEND) != 0;
    }

    void tie_set(uintptr_t base, bool on)
    {
        uint8_t const scr = r8(base + rs::SCR_OFFSET);
        if (on)
        {
            if ((scr & rs::SCR_TIE) == 0)
            {
                r8(base + rs::SCR_OFFSET) = static_cast<uint8_t>(scr | rs::SCR_TIE);
            }
            return;
        }
        if ((scr & rs::SCR_TIE) == 0)
        {
            return;
        }
        r8(base + rs::SCR_OFFSET) = static_cast<uint8_t>(scr & ~rs::SCR_TIE);
        // MANDATORY read-back: an RX I/O write is posted (UM sec.5 p.210), and sec.42.14.8
        // p.2317 spells out this sequence for SCR.TIE. The caller returns into kos_irq_wait,
        // which unmasks the line, so an unlanded TIE=0 rearms into a live source.
        //
        // BOUNDED: this runs on the drained-queue path in the only thread servicing the
        // device and the caller has no error channel, so giving up costs spurious wakes
        // rather than bytes, where an unbounded spin costs the console.
        for (uint32_t i = 0; i < POLL_MAX; i++)
        {
            if ((r8(base + rs::SCR_OFFSET) & rs::SCR_TIE) == 0)
            {
                return;
            }
        }
    }

    // The SMR framing bits, or false for a frame this controller cannot encode. CKS belongs
    // to the rate, so it is left to the caller below. Parity is an EXTRA bit on this part,
    // not a replacement for the eighth data bit (sec.42.2.9 p.2163), so 8 data bits plus
    // parity is an ordinary frame; 9-bit needs SCMR.CHR1 = 0 and would not fit an
    // unsigned char.
    bool encode_frame(struct kos_uart_config const* cfg, uint8_t* out_smr)
    {
        if (cfg->data_bits != 7u and cfg->data_bits != 8u)
        {
            return false;
        }
        if (cfg->stop_bits != 1u and cfg->stop_bits != 2u)
        {
            return false;
        }
        uint8_t smr = 0u;
        if (cfg->data_bits == 7u)
        {
            smr = static_cast<uint8_t>(smr | rs::SMR_CHR); // with SCMR.CHR1 = 1
        }
        if (cfg->stop_bits == 2u)
        {
            smr = static_cast<uint8_t>(smr | rs::SMR_STOP);
        }
        if (cfg->parity == KOS_UART_PARITY_EVEN)
        {
            smr = static_cast<uint8_t>(smr | rs::SMR_PE);
        }
        else if (cfg->parity == KOS_UART_PARITY_ODD)
        {
            smr = static_cast<uint8_t>(smr | rs::SMR_PE | rs::SMR_PM);
        }
        else if (cfg->parity != KOS_UART_PARITY_NONE)
        {
            return false;
        }
        *out_smr = smr;
        return true;
    }

    // The rate the generator is CURRENTLY producing, from the divisor READ BACK out of
    // SMR/SEMR/BRR, or a negative kos_errno.
    int32_t achieved_baud(uintptr_t base)
    {
        // PCLKB is selected and divided in the kernel-reserved SYSTEM block, so this
        // window's holder cannot read the tree. A 0 is a tree the chip could not describe.
        uint32_t const clk = kos_periph_clock_hz(base);
        if (clk == 0u)
        {
            return -KOS_ENOSYS;
        }
        uint8_t const semr = r8(base + rs::SEMR_OFFSET);
        if ((semr & rs::SEMR_BRME) != 0u)
        {
            // MDDR is at +0x12, outside the 16-byte window this class is granted, so a
            // modulated divisor cannot be read and must not be reported as unmodulated.
            return -KOS_ENOTSUP;
        }
        uint32_t const rate =
            rs::baud_achieved(clk, r8(base + rs::SMR_OFFSET), semr, r8(base + rs::BRR_OFFSET));
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
    uint8_t frame = 0u;
    if (not encode_frame(cfg, &frame))
    {
        return -KOS_ENOTSUP;
    }

    u->base = cfg->base;
    u->stats = cfg->stats;

    // MSTPCRB.SCI6 is released by arch_init, and the module-stop block is kernel-reserved.

    // Wait out the byte in flight: a divisor or frame change with a frame still shifting
    // corrupts it on the wire, and the kernel banner is normally still draining here.
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
    r8(u->base + rs::SCR_OFFSET) = 0u; // TE, RE, TIE, RIE off; CKE = 00 = on-chip BRG

    // Forced, not assumed: a restarted driver inherits what the last instance left, and
    // SMIF, SINV and SDIR each destroy every frame silently. CHR1 lands at 1, which is what
    // makes the 7- and 8-bit encodings above reachable from SMR.CHR alone.
    r8(u->base + rs::SCMR_OFFSET) = rs::SCMR_UART;

    if (cfg->baud == 0u)
    {
        // SMR carries the frame AND CKS, so CKS is read back rather than rewritten from a
        // request that was not made.
        uint8_t const cks = r8(u->base + rs::SMR_OFFSET) & rs::SMR_CKS_MASK;
        r8(u->base + rs::SMR_OFFSET) = static_cast<uint8_t>(frame | cks);
    }
    else
    {
        uint32_t const clk = kos_periph_clock_hz(u->base);
        if (clk == 0u)
        {
            return -KOS_ENOSYS; // a divisor off a guessed clock garbles the wire
        }
        kickos::rx::reg::sci::BaudSetting bs;
        if (not rs::baud_select(clk, cfg->baud, &bs))
        {
            return -KOS_ENOTSUP; // outside 0 <= BRR <= 255 for every legal divider
        }
        r8(u->base + rs::SMR_OFFSET) = static_cast<uint8_t>(frame | bs.cks);
        r8(u->base + rs::SEMR_OFFSET) = bs.semr; // BRME 0, so MDDR stays out of the rate
        r8(u->base + rs::BRR_OFFSET) = bs.brr;
    }

    // RIE carries ERI as well as RXI (sec.42.2.10 p.2167). TIE arms in kos_uart_write, on a
    // pass the device refused a byte.
    r8(u->base + rs::SCR_OFFSET) =
        static_cast<uint8_t>(rs::SCR_RIE | rs::SCR_TE | rs::SCR_RE);

    // Read BACK, not returned from the computation: BRR is an integer divisor and the wire
    // carries the rounded rate. A refusal must take the channel down, or it leaves a live
    // transmitter nobody owns.
    int32_t const rate = achieved_baud(u->base);
    if (rate < 0)
    {
        (void)kos_uart_close(u);
    }
    return rate;
}

uint32_t kos_uart_read(struct kos_uart* u, unsigned char* dst, uint32_t n)
{
    uint32_t got = 0;
    // Bounded by n, so a permanently-set status flag costs one pass rather than a hang.
    // Exiting with RDRF asserted loses nothing: RXI is EDGE, so the next byte re-raises.
    for (uint32_t i = 0; i < n; i++)
    {
        uint8_t const ssr = r8(u->base + rs::SSR_OFFSET);
        uint8_t const seen = static_cast<uint8_t>(ssr & rs::SSR_ERRORS);
        if (seen != 0u)
        {
            if ((seen & rs::SSR_ORER) != 0u)
            {
                kos_counter_increment(&u->stats->rx_overrun, 1u);
            }
            if ((seen & rs::SSR_FER) != 0u)
            {
                kos_counter_increment(&u->stats->rx_framing, 1u);
            }
            if ((seen & rs::SSR_PER) != 0u)
            {
                kos_counter_increment(&u->stats->rx_parity, 1u);
            }
            // Read RDR FIRST (Fig.42.21 step [6] p.2243) or the overrun is unrecoverable.
            // Through a named copy: `(void)r8(...)` would not perform the volatile access.
            unsigned char const bad = r8(u->base + rs::RDR_OFFSET);
            (void)bad;
            // Write 0 to exactly the bits that read 1, and 1 into the TDRE and RDRF
            // positions (Notes 1 and 2, p.2171).
            r8(u->base + rs::SSR_OFFSET) =
                static_cast<uint8_t>((ssr & ~seen) | rs::SSR_TDRE | rs::SSR_RDRF);
            continue;
        }
        if ((ssr & rs::SSR_RDRF) == 0u)
        {
            break;
        }
        // Reading RDR is what clears RDRF, so the loop makes its own progress.
        dst[got] = r8(u->base + rs::RDR_OFFSET);
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
        // ARMED BEFORE TDRE IS OBSERVED, AND THAT ORDER IS THE WHOLE RACE. Reading TDRE
        // first leaves a window where the pending transfer completes with TIE still 0, and
        // the pass then arms a source with no transition left to raise it.
        tie_set(u->base, true);
        if ((r8(u->base + rs::SSR_OFFSET) & rs::SSR_TDRE) == 0u)
        {
            break; // TDR busy: the transfer in flight IS the next raise
        }
        r8(u->base + rs::TDR_OFFSET) = src[i];
        i++;
    }
    // THIS CALL OWNS TIE. Left armed when the device refused a byte; disarmed on a call it
    // accepted whole, including the empty call a drained consumer ends its pass with.
    // Disarming also discards the request the SCI retains internally (sec.42.12.1(1)
    // p.2308), so it is legal only with nothing left to send.
    if (i >= n)
    {
        tie_set(u->base, false);
    }
    return i;
}

int32_t kos_uart_flush(struct kos_uart* u)
{
    // THE STRONG CONTRACT ON THIS PART: drained means the last stop bit has left the pin.
    // TEND covers the shift register, not just TDR (sec.42.2.11 p.2171).
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
    // One store clears TE, RE, TIE and RIE, which is every source this backend arms. In
    // asynchronous mode TE and RE are writable under any condition (sec.42.2.10 p.2167
    // Note 3). This TRUNCATES a frame still shifting, hence flush before close.
    r8(u->base + rs::SCR_OFFSET) = 0u;
    return 0;
}

}
