// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Infineon XMC4800 USIC shared-IP layer implementation. Protocol-independent
// mechanism only (see usic.h). Clean-room from the XMC4700/XMC4800 Reference
// Manual (V1.3, 2016-07); no XMCLib/DAVE/CMSIS vendor source. No unbounded
// polling loops here -- a misconfigured channel must never hang the caller.

#include "usic.h"

namespace kickos
{
namespace xmc
{
namespace usic
{
    void module_clock_enable(uintptr_t cgatclr, uintptr_t prclr, uint32_t bit)
    {
        reg32(cgatclr) = bit; // clear gating -> module clock on
        reg32(prclr) = bit;   // release the module out of reset
    }

    void kernel_clock_enable(uintptr_t base)
    {
        reg32(base + off::KSCFG) = KSCFG_MODEN | KSCFG_BPMODEN;
        // RM p.18-165 recommends reading KSCFG back before touching other USIC
        // registers to avoid control-block pipeline effects.
        uint32_t sync = reg32(base + off::KSCFG);
        __asm volatile("" : : "r"(sync) : "memory");
    }

    void set_baud(uintptr_t base, Baud const& b)
    {
        // Baud math (RM eq.18.6): fASC = fPIN / ((PDIV+1)*(PCTQ+1)*(DCTQ+1)),
        // with fPIN = fFD = fPERIPH * STEP/1024. A new baud is solved off-line
        // for the target fPERIPH and stored as a Baud constant (see usic.h).
        reg32(base + off::FDR) = FDR_DM_FRACTIONAL | (static_cast<uint32_t>(b.step) & FDR_STEP_MASK);
        reg32(base + off::BRG) = (static_cast<uint32_t>(b.pdiv) << BRG_PDIV_SHIFT)
                               | (static_cast<uint32_t>(b.dctq) << BRG_DCTQ_SHIFT)
                               | (static_cast<uint32_t>(b.pctq) << BRG_PCTQ_SHIFT);
    }

    void select_input(uintptr_t base, uintptr_t dxn_off, uint32_t dsel)
    {
        reg32(base + dxn_off) = dsel & DX_DSEL_MASK;
    }

    void configure_fifo(uintptr_t base, uintptr_t ctr_off, uint32_t dptr, uint32_t size, uint32_t limit)
    {
        reg32(base + ctr_off) = (dptr << FIFO_DPTR_SHIFT)
                              | (limit << FIFO_LIMIT_SHIFT)
                              | (size << FIFO_SIZE_SHIFT);
    }

    bool tx_ready(uintptr_t base)
    {
        return (reg32(base + off::TCSR) & TCSR_TDV) == 0;
    }

    void tx_put(uintptr_t base, uint8_t v)
    {
        reg32(base + off::TBUF0) = v;
    }

    bool tx_idle(uintptr_t base)
    {
        return (reg32(base + off::PSR) & PSR_BUSY) == 0;
    }

    void tx_irq_route(uintptr_t base, uint32_t srx)
    {
        uint32_t inpr = reg32(base + off::INPR);
        inpr &= ~INPR_TBINP_MASK;
        inpr |= (srx << INPR_TBINP_SHIFT) & INPR_TBINP_MASK;
        reg32(base + off::INPR) = inpr;
    }

    // RMW on CCR (which also holds MODE): only ever called by the console producer
    // under IrqLock or by its drain ISR, never concurrently, so the RMW is safe.
    void tx_irq_enable(uintptr_t base)
    {
        reg32(base + off::CCR) |= CCR_TBIEN;
    }

    void tx_irq_disable(uintptr_t base)
    {
        reg32(base + off::CCR) &= ~CCR_TBIEN;
    }

    bool rx_ready(uintptr_t base)
    {
        return (reg32(base + off::RBUFSR) & (RBUFSR_RDV0 | RBUFSR_RDV1)) != 0;
    }

    uint8_t rx_get(uintptr_t base)
    {
        // Reading RBUF returns the current standard buffer word and releases it,
        // clearing its RBUFSR.RDVx (RM p.18-202).
        return static_cast<uint8_t>(reg32(base + off::RBUF) & RBUF_DSR_MASK);
    }

    uint32_t status_read_clear(uintptr_t base, uint32_t mask)
    {
        // PSR flags are cleared by writing a 1 to the matching PSCR bit (RM
        // p.18-171); PSR bits are not auto-cleared by hardware.
        uint32_t set = reg32(base + off::PSR) & mask;
        if (set != 0)
        {
            reg32(base + off::PSCR) = set;
        }
        return set;
    }
}
}
}
