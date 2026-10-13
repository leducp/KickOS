// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Infineon XMC4800 USIC shared-IP layer (RM V1.3), protocol-independent. No unbounded polling
// loops: a misconfigured channel must never hang the caller.

#include "usic.h"

namespace kickos
{
namespace xmc
{
namespace usic
{
    void module_clock_enable(uintptr_t cgatclr, uintptr_t prclr, uint32_t bit)
    {
        r32(cgatclr) = bit;
        r32(prclr) = bit;
    }

    void kernel_clock_enable(uintptr_t base)
    {
        r32(base + off::KSCFG) = KSCFG_MODEN | KSCFG_BPMODEN;
        // Read back before any other USIC register, against pipeline effects (RM p.18-165).
        uint32_t sync = r32(base + off::KSCFG);
        __asm volatile("" : : "r"(sync) : "memory");
    }

    void set_baud(uintptr_t base, Baud const& b)
    {
        // fASC = fPIN / ((PDIV+1)*(PCTQ+1)*(DCTQ+1)), fPIN = fPERIPH * STEP/1024 (RM eq.18.6).
        r32(base + off::FDR) = FDR_DM_FRACTIONAL | (static_cast<uint32_t>(b.step) & FDR_STEP_MASK);
        r32(base + off::BRG) = (static_cast<uint32_t>(b.pdiv) << BRG_PDIV_SHIFT)
                               | (static_cast<uint32_t>(b.dctq) << BRG_DCTQ_SHIFT)
                               | (static_cast<uint32_t>(b.pctq) << BRG_PCTQ_SHIFT);
    }

    bool baud_for_periph(uint32_t periph_hz, Baud* out)
    {
        return baud_115200_for(periph_hz, out);
    }

    void select_input(uintptr_t base, uintptr_t dxn_off, uint32_t dsel)
    {
        r32(base + dxn_off) = dsel & DX_DSEL_MASK;
    }

    void configure_fifo(uintptr_t base, uintptr_t ctr_off, uint32_t dptr, uint32_t size, uint32_t limit)
    {
        r32(base + ctr_off) = (dptr << FIFO_DPTR_SHIFT)
                              | (limit << FIFO_LIMIT_SHIFT)
                              | (size << FIFO_SIZE_SHIFT);
    }

    bool tx_ready(uintptr_t base)
    {
        return (r32(base + off::TCSR) & TCSR_TDV) == 0;
    }

    void tx_put(uintptr_t base, uint8_t v)
    {
        r32(base + off::TBUF0) = v;
    }

    bool tx_idle(uintptr_t base)
    {
        return (r32(base + off::PSR) & PSR_BUSY) == 0;
    }

    void tx_irq_route(uintptr_t base, uint32_t srx)
    {
        uint32_t inpr = r32(base + off::INPR);
        inpr &= ~INPR_TBINP_MASK;
        inpr |= (srx << INPR_TBINP_SHIFT) & INPR_TBINP_MASK;
        r32(base + off::INPR) = inpr;
    }

    // An unsynchronised RMW on CCR, which also holds MODE: not for concurrent entry.
    void tx_irq_enable(uintptr_t base)
    {
        r32(base + off::CCR) |= CCR_TBIEN;
    }

    void tx_irq_disable(uintptr_t base)
    {
        r32(base + off::CCR) &= ~CCR_TBIEN;
    }
}
}
}
