// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The XMC4800 USIC0-CH1 SSC backend of the SPI class <kickos/driver/spi.h>: the whole channel
// bring-up plus the IRQ-paced transaction engine, for a thread that ALREADY HOLDS the U0C1
// register window and the USIC0 SR1 line cap.
//
// THE DATA PATH IS INTERNAL LOOP-BACK (DX0 = own transmitter, input "G", RM 18.2.3.5), so
// rx == tx on-chip and SELO0 is armed but never routed to a pin. A board that wires this
// channel to a real device needs that pin-mux and a DX0 selection change.
//
// FDR, BRG and CCR are Write = PV at the bus and an unprivileged store to them is silently
// discarded with NO fault (measured: user/apps/xmc4800-relax/pvprobe), so those three go
// through kos_periph_reg_write and are READ BACK; a discarded store is invisible otherwise.
//
// Register addresses / bit fields are clean-room from the XMC4700/XMC4800 Reference Manual
// (V1.3, 2016-07); no XMCLib/DAVE/CMSIS vendor source. "RM p.NN" are the manual's printed
// pages, "RM n.n.n" its printed section numbers.

#include <kickos/driver/spi.h>

#include <kickos/io/mmio.h> // r32
#include <kickos/sys.h>     // kos_periph_reg_write, kos_periph_clock_hz, kos_irq_wait/ack
#include <kickos/sys/errno.h>

#include <regs/usic.h>

#include <stdint.h>

namespace
{
    namespace ru = kickos::xmc::reg::usic;

    // The fixed 72 MHz baud profile this channel is brought up on. A device cannot retune it:
    // BRG also pins the clock polarity and phase, so refolding it per device would change
    // every other device's frame mid-transaction.
    constexpr uint32_t FDR_WORD = ru::FDR_DM_FRACTIONAL | ru::FDR_STEP_367;
    constexpr uint32_t BRG_WORD = ru::BRG_PDIV_13 | ru::BRG_DCTQ_15 | ru::BRG_PCTQ_0;
    constexpr uint32_t CCR_WORD = ru::CCR_MODE_SSC | ru::CCR_RIEN | ru::CCR_AIEN;

    // SCTR field positions for the word size (FLE/WLE are N+1).
    constexpr unsigned SCTR_FLE_SHIFT = 16u;
    constexpr unsigned SCTR_WLE_SHIFT = 24u;

    // TCSR frame-control base (start-on-TDV + single-shot); SOF/EOF added per word.
    constexpr uint32_t TCSR_BASE = ru::TCSR_TDEN_TDV | ru::TCSR_TDSSM;

    // FEM holds the CS across a multi-word frame (RM 18.4.5.1).
    constexpr uint32_t PCR_CS_HW = ru::PCR_MSLSEN | ru::PCR_SELCTR_DIRECT | ru::PCR_SELINV_LOW
                                 | ru::PCR_SELO0 | ru::PCR_FEM;

    // Clear both RX-complete flags (a single-word frame lands in AIF, RM 18.4.2.7).
    constexpr uint32_t PSCR_CLEAR_RX = ru::PSCR_CRIF | ru::PSCR_CAIF;

    // The one frame size this engine clocks. The class buffer is one byte per frame, so a
    // wider word would leave the top bits of every frame unsourced.
    constexpr uint8_t FRAME_WORD_BITS = 8u;

    // FDR/BRG read back exactly what was written apart from FDR.RESULT[25:16], which the
    // fractional divider drives; BRG carries no read-only field in the bits this profile
    // sets.
    constexpr uint32_t FDR_RESULT_MASK = 0x03FF0000u;

    // kos_spi_device.prog slots.
    constexpr unsigned PROG_SCTR = 0u;
    constexpr unsigned PROG_PCR = 1u;

    // A dropped store leaves the register at its previous value with NO fault, so the
    // read-back is the only evidence either way.
    bool priv_write_verify(uintptr_t win, uintptr_t offset, uint32_t value, uint32_t care)
    {
        if (kos_periph_reg_write(win, offset, value) != 0)
        {
            return false;
        }
        return (r32(win + offset) & care) == (value & care);
    }

    // The SSC shift clock the baud generator is CURRENTLY producing, read back out of FDR +
    // BRG, or a negative kos_errno. Both are readable unprivileged (RM Table 18-20, Read =
    // "U, PV"); only their write side is privileged.
    //
    // RM eq.18.8 off the fractional divider of RM eq.18.2: fSCLK = fPERIPH * STEP/1024 /
    // ((PDIV+1) * (PCTQ+1) * 2 * (DCTQ+1)). That form holds ONLY for DM = fractional with the
    // three divider-chain selects at 0, so both are checked: a chain this backend cannot
    // express is ENOTSUP, not a rate reported four times too high.
    int32_t achieved_hz(uintptr_t win)
    {
        uint32_t const fperiph = kos_periph_clock_hz(win);
        if (fperiph == 0u)
        {
            return -KOS_ENOSYS; // no branch-clock oracle for this block: the rate is unknowable
        }
        uint32_t const fdr = r32(win + ru::off::FDR);
        if ((fdr & ru::FDR_DM_MASK) != ru::FDR_DM_FRACTIONAL)
        {
            return -KOS_ENOTSUP;
        }
        uint32_t const brg = r32(win + ru::off::BRG);
        if ((brg & ru::BRG_CHAIN_EQ186_MASK) != 0u)
        {
            return -KOS_ENOTSUP;
        }
        uint64_t const step = fdr & ru::FDR_STEP_MASK;
        uint64_t const pdiv = ((brg & ru::BRG_PDIV_MASK) >> ru::BRG_PDIV_SHIFT) + 1u;
        uint64_t const pctq = ((brg & ru::BRG_PCTQ_MASK) >> ru::BRG_PCTQ_SHIFT) + 1u;
        uint64_t const dctq = ((brg & ru::BRG_DCTQ_MASK) >> ru::BRG_DCTQ_SHIFT) + 1u;
        uint64_t const rate =
            (static_cast<uint64_t>(fperiph) * step) / (1024u * pdiv * pctq * 2u * dctq);
        if (rate == 0u)
        {
            return -KOS_ENOTSUP; // STEP=0 stops the divider: no rate to report
        }
        return static_cast<int32_t>(rate);
    }
}

extern "C"
{

int32_t kos_spi_bus_open(struct kos_spi_bus* b, struct kos_spi_bus_config const* cfg)
{
    if (cfg->base == 0u)
    {
        return -KOS_EINVAL;
    }
    if (cfg->irq == KOS_CAP_NONE)
    {
        return -KOS_EINVAL; // the engine blocks on the per-word RX-complete line
    }
    b->base = cfg->base;
    b->ep = cfg->ep;
    b->irq = cfg->irq;

    uintptr_t const win = b->base;

    // The RM fixes the ORDER: KSCFG first (with MODEN=0 the channel is inaccessible except
    // through KSCFG), then every configuration register while CCR.MODE=0, then CCR last.
    r32(win + ru::off::KSCFG) = ru::KSCFG_MODEN | ru::KSCFG_BPMODEN;
    // RM p.18-165: KSCFG must be read back before any other USIC register is touched, to
    // flush the control-block pipeline. The asm keeps that read from being elided.
    uint32_t const kscfg_sync = r32(win + ru::off::KSCFG);
    __asm volatile("" : : "r"(kscfg_sync) : "memory");

    if (not priv_write_verify(win, ru::off::FDR, FDR_WORD, ~FDR_RESULT_MASK))
    {
        return -KOS_EPERM;
    }
    if (not priv_write_verify(win, ru::off::BRG, BRG_WORD, 0xFFFFFFFFu))
    {
        return -KOS_EPERM;
    }

    // Seed only: kos_spi_transfer rewrites SCTR/PCR from the named device's profile, and a
    // transfer needs a device handle, so nothing ever clocks on these values.
    r32(win + ru::off::SCTR) = ru::SCTR_TRM_ACTIVE | (7u << SCTR_WLE_SHIFT) | ru::SCTR_SDIR_MSB
                             | (7u << SCTR_FLE_SHIFT);
    r32(win + ru::off::TCSR) = TCSR_BASE;
    r32(win + ru::off::PCR) = ru::PCR_MSLSEN;
    r32(win + ru::off::PSCR) = ru::PSCR_MSLS | PSCR_CLEAR_RX; // clear stale flags

    // Internal loop-back: DX0 receives the channel's own transmitter (input "G"). Input-stage
    // config must be done while CCR.MODE=0 (RM p.18-57).
    r32(win + ru::off::DX0CR) = ru::DX0CR_INSW | ru::DX0CR_DSEL_G;

    // Receive / alternative-receive interrupts to service-request SR1.
    r32(win + ru::off::INPR) = ru::INPR_RINP_SR1 | ru::INPR_AINP_SR1;

    if (not priv_write_verify(win, ru::off::CCR, CCR_WORD, 0xFFFFFFFFu))
    {
        return -KOS_EPERM;
    }
    return 0;
}

int32_t kos_spi_device_open(struct kos_spi_device* d, struct kos_spi_bus* b,
                            struct kos_spi_device_config const* cfg)
{
    if (b->base == 0u)
    {
        return -KOS_EINVAL; // no open bus behind this handle
    }
    if (cfg->slot >= KOS_BUS_DEV_MAX)
    {
        return -KOS_EINVAL;
    }
    if (cfg->rsv[0] != 0u or cfg->rsv[1] != 0u or cfg->rsv[2] != 0u)
    {
        return -KOS_EINVAL;
    }
    if ((cfg->mode & ~static_cast<uint8_t>(KOS_BUS_MODE_CPOL | KOS_BUS_MODE_CPHA
                                          | KOS_BUS_MODE_LSB_FIRST)) != 0u)
    {
        return -KOS_EINVAL;
    }

    // REFUSED FOR THIS CHANNEL'S PROFILE, NOT FOR THE SEAM: FDR/BRG are reachable through
    // kos_periph_reg_write, but BRG also carries SCLKCFG (the clock polarity and phase) and
    // one live divider serves every device on the channel, so a per-device retune would
    // silently reframe the others.
    if (cfg->hz != 0u)
    {
        return -KOS_ENOTSUP;
    }
    if ((cfg->mode & (KOS_BUS_MODE_CPOL | KOS_BUS_MODE_CPHA)) != 0u)
    {
        return -KOS_ENOTSUP; // BRG.SCLKCFG is pinned to SPI mode 0 by kos_spi_bus_open
    }

    uint8_t bits = cfg->word_bits;
    if (bits == 0u)
    {
        bits = FRAME_WORD_BITS;
    }
    if (bits != FRAME_WORD_BITS)
    {
        return -KOS_ENOTSUP;
    }

    // ONE CS LINE, SELO0: a non-zero cs_index is a line this channel does not arm.
    if (cfg->cs_index != 0u)
    {
        return -KOS_ENOTSUP;
    }
    if (cfg->cs_policy == KOS_BUS_CS_GPIO)
    {
        return -KOS_ENOTSUP; // no GPIO belongs to this channel's grant
    }
    if (cfg->cs_policy != KOS_BUS_CS_HW and cfg->cs_policy != KOS_BUS_CS_NONE)
    {
        return -KOS_EINVAL;
    }
    bool cs_hw = false;
    if (cfg->cs_policy == KOS_BUS_CS_HW)
    {
        cs_hw = true;
    }

    uint32_t sctr = ru::SCTR_TRM_ACTIVE
                  | ((static_cast<uint32_t>(bits - 1u) & 0xFu) << SCTR_WLE_SHIFT);
    if (cs_hw)
    {
        // FLE=63: the frame is NOT terminated by a bit count, the software TCSR.SOF/EOF
        // markers govern the frame end (RM 18.4.3.6), so MSLS brackets every word the
        // transfer feeds.
        sctr |= ru::SCTR_FLE_63;
    }
    else
    {
        sctr |= (static_cast<uint32_t>(bits - 1u) & 0x3Fu) << SCTR_FLE_SHIFT; // one word per frame
    }
    if ((cfg->mode & KOS_BUS_MODE_LSB_FIRST) == 0u)
    {
        sctr |= ru::SCTR_SDIR_MSB;
    }

    int32_t const rate = achieved_hz(b->base);
    if (rate < 0)
    {
        return rate;
    }

    d->bus = b;
    d->hz = static_cast<uint32_t>(rate);
    d->prog[PROG_SCTR] = sctr;
    d->prog[PROG_PCR] = ru::PCR_MSLSEN;
    if (cs_hw)
    {
        d->prog[PROG_PCR] = PCR_CS_HW;
    }
    d->slot = cfg->slot;
    d->mode = cfg->mode;
    d->word_bits = bits;
    d->cs_policy = cfg->cs_policy;
    d->cs_index = cfg->cs_index;
    d->rsv[0] = 0u;
    d->rsv[1] = 0u;
    d->rsv[2] = 0u;
    return rate;
}

int32_t kos_spi_transfer(struct kos_spi_device* d, struct kos_bus_seg const* seg, uint8_t nseg,
                         unsigned char* buf, uint32_t len)
{
    int32_t const bad = kos_spi_seg_check(seg, nseg, len);
    if (bad != 0)
    {
        return bad;
    }
    struct kos_spi_bus* const b = d->bus;
    if (b == nullptr or b->base == 0u)
    {
        return -KOS_EINVAL;
    }

    uintptr_t const win = b->base;
    kos_cap_t const irq = b->irq;
    bool const cs_hw = (d->cs_policy == KOS_BUS_CS_HW);

    r32(win + ru::off::SCTR) = d->prog[PROG_SCTR];
    r32(win + ru::off::PCR) = d->prog[PROG_PCR];

    volatile uint32_t* tcsr = reinterpret_cast<volatile uint32_t*>(win + ru::off::TCSR);
    volatile uint32_t* pscr = reinterpret_cast<volatile uint32_t*>(win + ru::off::PSCR);
    volatile uint32_t* rbuf = reinterpret_cast<volatile uint32_t*>(win + ru::off::RBUF);
    volatile uint32_t* tbuf0 = reinterpret_cast<volatile uint32_t*>(win + ru::off::TBUF0);

    for (uint32_t i = 0; i < len; i++)
    {
        // Frame markers must be set BEFORE the TBUF write; they are sampled when TDV goes
        // valid. Only meaningful for CS_HW (FLE=63), harmless otherwise.
        uint32_t tcsr_val = TCSR_BASE;
        if (cs_hw)
        {
            if (i == 0u)
            {
                tcsr_val |= ru::TCSR_SOF;
            }
            if (i == (len - 1u))
            {
                tcsr_val |= ru::TCSR_EOF;
            }
        }
        *tcsr = tcsr_val;

        *tbuf0 = static_cast<uint32_t>(buf[i]) & 0xFFu; // TDV=1 -> clock one frame

        kos_irq_wait(irq);                                  // block until AIF/RIF raises SR1
        buf[i] = static_cast<unsigned char>(*rbuf & 0xFFu); // read releases RBUF

        *pscr = PSCR_CLEAR_RX; // W1C AIF/RIF BEFORE re-arm: an un-cleared level re-asserts
                               // SR1 on unmask and storms it.
        kos_irq_ack(irq);      // unmask the line (flag already clear -> no storm)
    }

    if (cs_hw)
    {
        *pscr = ru::PSCR_MSLS; // defensively force MSLS inactive after the EOF word
    }
    return static_cast<int32_t>(len);
}

int32_t kos_spi_bus_close(struct kos_spi_bus* b)
{
    if (b->base == 0u)
    {
        return 0; // never opened, or already closed
    }
    uintptr_t const win = b->base;

    // CCR = 0 in ONE word: MODE[3:0] = 0 leaves the SSC protocol, stopping the shift clock,
    // and the same store clears RIEN/AIEN so no receive source stays armed. Absolute rather
    // than read-modify-write because the seam stores whole words.
    if (kos_periph_reg_write(win, ru::off::CCR, 0u) != 0)
    {
        return -KOS_EPERM;
    }
    if (r32(win + ru::off::CCR) != 0u)
    {
        return -KOS_EPERM;
    }
    r32(win + ru::off::PSCR) = ru::PSCR_MSLS | PSCR_CLEAR_RX; // CS idle, flags clear
    b->base = 0u;
    return 0;
}

}
