// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 USIC backend of <kickos/driver/uart.h>, for a channel kickos_xmc_usic_init has
// already clocked, pinned and given a divisor.
//
// Register addresses / bit fields are clean-room from the XMC4700/XMC4800 Reference Manual
// (V1.3, 2016-07); no XMCLib/DAVE/CMSIS vendor source. "RM p.NN" are the manual's printed
// pages, "RM n.n.n" its printed section numbers.

#include <kickos/driver/uart.h>

#include <kickos/io/mmio.h>   // r32
#include <kickos/sys.h>       // kos_periph_reg_write, kos_periph_clock_hz
#include <kickos/sys/errno.h> // KOS_EPERM, KOS_ENOSYS, KOS_ENOTSUP, KOS_EBUSY

#include <regs/usic.h>
#include <usic_class.h> // usic_tx_ready

#include <stdint.h>

namespace
{
    namespace ru = kickos::xmc::reg::usic;

    // MODE must be RESTATED alongside TBIEN: the seam stores the whole 32-bit word absolutely
    // and never read-modify-writes, so a bare TBIEN would clear MODE[3:0] and disable the
    // channel.
    constexpr uint32_t CCR_WORD = ru::CCR_MODE_ASC | ru::CCR_TBIEN;

    // What kickos_xmc_usic_init programmed into SCTR (WLE/FLE = 8 bits) and PCR (no parity,
    // one stop bit).
    constexpr uint8_t FRAME_DATA_BITS = 8u;
    constexpr uint8_t FRAME_STOP_BITS = 1u;

    constexpr uint32_t FLUSH_POLL_MAX = 1000000u;

    // fASC = fPERIPH * STEP/1024 / ((PDIV+1) * (PCTQ+1) * (DCTQ+1)) (RM eq.18.6, RM 18.3.3.2
    // p.18-60; fractional divider RM eq.18.2 p.18-27). That form holds only for DM =
    // fractional and the three divider-chain selects at 0, so both are checked: a chain this
    // does not express would report a rate four times too high.
    //
    // Truncating, so the answer is the achieved rate rounded DOWN: the 72 MHz 115200 point is
    // 115199.5 baud on the wire and reports 115199.
    int32_t achieved_baud(uintptr_t base)
    {
        uint32_t const fperiph = kos_periph_clock_hz(base);
        if (fperiph == 0u)
        {
            return -KOS_ENOSYS;
        }
        uint32_t const fdr = r32(base + ru::off::FDR);
        if ((fdr & ru::FDR_DM_MASK) != ru::FDR_DM_FRACTIONAL)
        {
            return -KOS_ENOTSUP;
        }
        uint32_t const brg = r32(base + ru::off::BRG);
        if ((brg & ru::BRG_CHAIN_EQ186_MASK) != 0u)
        {
            return -KOS_ENOTSUP;
        }
        uint64_t const step = fdr & ru::FDR_STEP_MASK;
        uint64_t const pdiv = ((brg & ru::BRG_PDIV_MASK) >> ru::BRG_PDIV_SHIFT) + 1u;
        uint64_t const pctq = ((brg & ru::BRG_PCTQ_MASK) >> ru::BRG_PCTQ_SHIFT) + 1u;
        uint64_t const dctq = ((brg & ru::BRG_DCTQ_MASK) >> ru::BRG_DCTQ_SHIFT) + 1u;
        uint64_t const rate = (static_cast<uint64_t>(fperiph) * step) / (1024u * pdiv * pctq * dctq);
        if (rate == 0u)
        {
            return -KOS_ENOTSUP; // STEP=0 stops the divider
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
    // FDR and BRG are Write = PV (RM Table 18-20, p.18-155) and the seam's allowlist carries
    // a CCR entry only for U0C0, so the kernel's divisor stands. -KOS_ENOTSUP and not the
    // -KOS_EPERM the two CCR read-backs below use: no store is attempted here, so a consumer
    // must not read this as a grant that could have been widened.
    int32_t const fixed_rate = kos_uart_cfg_check_fixed_rate(cfg);
    if (fixed_rate != 0)
    {
        return fixed_rate;
    }
    if (cfg->data_bits != FRAME_DATA_BITS or cfg->parity != KOS_UART_PARITY_NONE or
        cfg->stop_bits != FRAME_STOP_BITS)
    {
        return -KOS_ENOTSUP;
    }

    u->base = cfg->base;
    u->stats = cfg->stats;

    // CCR is Write = PV: an unprivileged store is discarded by the bus with NO fault, so the
    // read-back is the only evidence either way.
    if (kos_periph_reg_write(u->base, ru::off::CCR, CCR_WORD) != 0)
    {
        return -KOS_EPERM;
    }
    if (r32(u->base + ru::off::CCR) != CCR_WORD)
    {
        return -KOS_EPERM;
    }
    int32_t const rate = achieved_baud(u->base);
    if (rate < 0)
    {
        (void)kos_uart_close(u); // a refused open must not leave the channel enabled
    }
    return rate;
}

uint32_t kos_uart_read(struct kos_uart*, unsigned char*, uint32_t)
{
    // TX-only channel: CCR_WORD arms MODE_ASC and TBIEN alone, so nothing latches a received
    // byte and 0 is the true count.
    return 0;
}

uint32_t kos_uart_write(struct kos_uart* u, unsigned char const* src, uint32_t n)
{
    // TBIEN stays as CCR_WORD left it: the transmit-buffer event is edge-per-word, raised
    // when a word is loaded from TBUF into the shift register (RM 18.2.2.4, p.18-18;
    // ASC-specific p.18-64), so an idle channel raises nothing.
    //
    // TCSR.TDV == 1 means TBUF still HOLDS a word (RM p.18-189), so a store without testing
    // it first overwrites the byte still queued.
    uint32_t i = 0;
    while (i < n)
    {
        if (not kickos::xmc::driver::usic_tx_ready(u->base))
        {
            break;
        }
        r32(u->base + ru::off::TBUF0) = static_cast<uint32_t>(src[i]);
        i++;
    }
    return i;
}

int32_t kos_uart_flush(struct kos_uart* u)
{
    // THE STRONG CONTRACT ON THIS PART: drained means the last stop bit has left the pin.
    // PSR.BUSY is set from the start-of-frame bit to the end of the last stop bit (RM
    // 18.3.3.16, p.18-65; field p.18-71), and PCR.TSTEN routes transmit status into it.
    //
    // BOTH BITS ARE REQUIRED: BUSY's window starts at the start bit, so a word already in
    // TBUF that has not begun shifting reads BUSY=0 while TCSR.TDV=1.
    for (uint32_t i = 0; i < FLUSH_POLL_MAX; i++)
    {
        uint32_t const tcsr = r32(u->base + ru::off::TCSR);
        uint32_t const psr = r32(u->base + ru::off::PSR);
        if ((tcsr & ru::TCSR_TDV) == 0u and (psr & ru::PSR_BUSY) == 0u)
        {
            return 0;
        }
    }
    return -KOS_EBUSY;
}

int32_t kos_uart_close(struct kos_uart* u)
{
    // TRUNCATES a frame still shifting (RM 18.3.3.8/18.3.3.9, p.18-63), which is why the
    // class contract puts flush before close.
    if (kos_periph_reg_write(u->base, ru::off::CCR, 0u) != 0)
    {
        return -KOS_EPERM;
    }
    if (r32(u->base + ru::off::CCR) != 0u)
    {
        return -KOS_EPERM;
    }
    return 0;
}

}
