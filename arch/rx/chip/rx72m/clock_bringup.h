// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ICLK onto the 24 MHz crystal's PLL at 240 MHz, PCLKB at 60 MHz (RX72M UM sec.9), the 20 MHz
// HOCO if the crystal or the PLL never comes up, and the rates the live tree runs at.

#ifndef KICKOS_ARCH_RX_CHIP_RX72M_CLOCK_BRINGUP_H
#define KICKOS_ARCH_RX_CHIP_RX72M_CLOCK_BRINGUP_H

#include <kickos/io/mmio.h>

#include <stdint.h>

#include "regs/cgc.h"

namespace kickos::rx::clocktree
{
    namespace cgc = reg::cgc;

    // A clock misconfiguration must degrade, never hang the boot. Sized for the LOCO-clocked worst
    // case (settling counts run off the ~240 kHz LOCO).
    constexpr uint32_t CLOCK_POLL_LIMIT = 2000000u;

    static inline bool poll_flag(uintptr_t reg, uint8_t mask, uint32_t limit)
    {
        for (uint32_t i = 0; i < limit; i++)
        {
            if ((r8(reg) & mask) != 0)
            {
                return true;
            }
        }
        return false;
    }

    // ICLK to 240 MHz via the PLL (UM sec.9.6 case 1). Returns false, leaving the chip on
    // the LOCO, if the oscillator or PLL never reports stable. Runs inside the PRCR unlock.
    static inline bool clock_to_pll_240mhz()
    {
        r8(cgc::MOFCR) = cgc::MOFCR_XTAL_24MHZ;
        r8(cgc::MOSCWTCR) = cgc::MOSCWTCR_MSTS;
        r8(cgc::MOSCCR) = cgc::MOSCCR_MAIN_RUN;
        uint8_t mosccr_rb = r8(cgc::MOSCCR); // read back before dependent writes (sec.9.2.8)
        (void)mosccr_rb;
        if (not poll_flag(cgc::OSCOVFSR, cgc::OSCOVFSR_MOOVF, CLOCK_POLL_LIMIT))
        {
            return false;
        }
        // One flash wait state BEFORE the PLL runs (sec.9.8 case 1), read back so it holds
        // before ICLK can rise. Legal at any ICLK (Table 9.3).
        r8(cgc::MEMWAIT) = cgc::MEMWAIT_ONE_WAIT;
        uint8_t memwait_rb = r8(cgc::MEMWAIT);
        (void)memwait_rb;
        r16(cgc::PLLCR) = cgc::PLLCR_PLL_240MHZ;
        r8(cgc::PLLCR2) = cgc::PLLCR2_PLL_RUN;
        if (not poll_flag(cgc::OSCOVFSR, cgc::OSCOVFSR_PLOVF, CLOCK_POLL_LIMIT))
        {
            return false;
        }
        // Set the dividers BEFORE the source switch: while still on the LOCO the
        // divisors apply to ~240 kHz, so no peripheral overshoots when the PLL
        // becomes the source on the next write.
        r32(cgc::SCKCR) = cgc::SCKCR_240MHZ;
        r16(cgc::SCKCR3) = cgc::SCKCR3_CKSEL_PLL;
        return true;
    }

    // The fallback for a dead crystal or PLL: the 20 MHz HOCO, on the reset SCKCR dividers (all
    // /1), per UM sec.9.6 (4). False leaves the chip on the LOCO, where no console baud lands.
    static inline bool clock_to_hoco()
    {
        if ((r8(cgc::HOCOCR) & cgc::HOCOCR_HCSTP) != 0)
        {
            r8(cgc::HOCOCR2) = cgc::HOCOCR2_HCFRQ_20MHZ;
            r8(cgc::HOCOCR) = 0;
        }
        if (not poll_flag(cgc::OSCOVFSR, cgc::OSCOVFSR_HCOVF, CLOCK_POLL_LIMIT))
        {
            return false;
        }
        r16(cgc::SCKCR3) = static_cast<uint16_t>(cgc::CKSEL_HOCO << cgc::SCKCR3_CKSEL_S);
        return true;
    }

    // HOCO frequency, from HOCOCR2.HCFRQ (UM sec.9.2.13 p.364). Readable whatever
    // HOCOCR.HCSTP says; only the WRITE is gated on the HOCO being stopped.
    static inline uint32_t hoco_hz()
    {
        uint8_t const f = r8(cgc::HOCOCR2) & cgc::HOCOCR2_HCFRQ_MASK;
        if (f == 0)
        {
            return cgc::HOCO_16MHZ;
        }
        if (f == 1)
        {
            return cgc::HOCO_18MHZ;
        }
        if (f == 2)
        {
            return cgc::HOCO_20MHZ;
        }
        return 0; // 11b is prohibited: no frequency to report
    }

    // The clock the SCKCR dividers are fed from, read out of the LIVE tree rather than
    // assumed from what clock_to_pll_240mhz() intended. Returns 0 for a tree this cannot
    // describe, which every caller must forward rather than substitute a nominal for.
    static inline uint32_t clock_source_hz()
    {
        uint16_t const sel =
            (r16(cgc::SCKCR3) >> cgc::SCKCR3_CKSEL_S) & cgc::SCKCR3_CKSEL_MASK;
        if (sel == cgc::CKSEL_LOCO)
        {
            return cgc::LOCO_HZ;
        }
        if (sel == cgc::CKSEL_HOCO)
        {
            return hoco_hz();
        }
        if (sel == cgc::CKSEL_MAIN)
        {
            return cgc::MAIN_OSC_HZ;
        }
        if (sel == cgc::CKSEL_SUB)
        {
            return cgc::SUB_OSC_HZ;
        }
        if (sel != cgc::CKSEL_PLL)
        {
            return 0; // 101..111 are prohibited encodings
        }
        uint16_t const pllcr = r16(cgc::PLLCR);
        uint32_t in = cgc::MAIN_OSC_HZ;
        if ((pllcr & cgc::PLLCR_PLLSRCSEL) != 0)
        {
            in = hoco_hz();
        }
        uint32_t const plidiv = (pllcr & cgc::PLLCR_PLIDIV_MASK) + 1u;
        uint32_t const stc = (pllcr >> cgc::PLLCR_STC_S) & cgc::PLLCR_STC_MASK;
        if (in == 0u or plidiv > 3u or stc < cgc::PLLCR_STC_MIN or stc > cgc::PLLCR_STC_MAX)
        {
            return 0;
        }
        // The multiplier runs in HALF steps, so it is applied as (STC+1)/2 with the divide
        // last: rounding it to an integer first would drop 500 kHz per PLL input MHz.
        uint64_t const out = static_cast<uint64_t>(in) * (stc + 1u) / (2u * plidiv);
        return static_cast<uint32_t>(out);
    }

    // PCLKB, the SCI and CMTW clock (UM sec.42 preamble p.2144 for the SCI).
    static inline uint32_t pclkb_hz()
    {
        uint32_t const src = clock_source_hz();
        uint32_t const pckb = (r32(cgc::SCKCR) >> cgc::SCKCR_PCKB_S) & cgc::SCKCR_DIV_MASK;
        if (src == 0u or pckb > cgc::SCKCR_DIV_MAX)
        {
            return 0; // 0111..1111 are prohibited division ratios
        }
        return src >> pckb;
    }

    // ICLK off the live tree, 0 for a tree clock_source_hz() cannot describe.
    static inline uint32_t iclk_hz()
    {
        uint32_t const src = clock_source_hz();
        uint32_t const ick = (r32(cgc::SCKCR) >> cgc::SCKCR_ICK_S) & cgc::SCKCR_DIV_MASK;
        if (src == 0u or ick > cgc::SCKCR_DIV_MAX)
        {
            return 0u;
        }
        return src >> ick;
    }

    // The PLL, or the HOCO if the crystal or the PLL never reports stable. Runs inside the PRCR
    // unlock; the rate it landed on is read back from the tree.
    static inline void bring_up()
    {
        if (not clock_to_pll_240mhz())
        {
            (void)clock_to_hoco();
        }
    }

}

#endif
