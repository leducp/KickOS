// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// fSYS = fCPU onto the 12 MHz crystal's system PLL at 144 MHz, fPERIPH at 72 MHz (XMC4700/XMC4800
// RM V1.3, ch.11). A crystal or PLL that never comes up leaves the CPU on the ~24 MHz fOFI.

#ifndef KICKOS_ARCH_ARM_CHIP_XMC4800_CLOCK_BRINGUP_H
#define KICKOS_ARCH_ARM_CHIP_XMC4800_CLOCK_BRINGUP_H

#include <kickos/arch/bounded_wait.h>
#include <kickos/io/mmio.h>

#include <stdint.h>

#include "regs/flash.h"
#include "regs/scu.h"

namespace kickos::xmc::clocktree
{
    namespace scu = reg::scu;
    namespace flash = reg::flash;

    constexpr uint32_t FOFI_HZ = 24000000u;
    constexpr uint32_t FCPU_PLL_HZ = 144000000u;

    // fVCO = 12 MHz * NDIV/PDIV = 288 MHz; fPLL = fVCO/K2DIV. Each PLLCON1 field is written as
    // (value-1).
    static inline uint32_t pllcon1_value(uint32_t k2div)
    {
        return ((scu::PLL_NDIV - 1u) << scu::PLLCON1_NDIV_SHIFT)
             | ((k2div - 1u) << scu::PLLCON1_K2DIV_SHIFT)
             | ((scu::PLL_PDIV - 1u) << scu::PLLCON1_PDIV_SHIFT);
    }

    // Lets a K2DIV step settle (~50 us).
    static inline void clock_delay()
    {
        // Volatile so -Os keeps the wait.
        for (volatile uint32_t i = 0; i < 8000u;)
        {
            i = i + 1;
            __asm volatile("nop");
        }
    }

    // Flash read wait-states: an fCPU increase past the current setting's access window makes a
    // fetch return before the data is valid, a fault and not merely wrong timing (RM 8.4.4).
    // Widen before a rise, relax after a fall.
    static inline void set_flash_ws(uint32_t ws)
    {
        uint32_t fcon = r32(flash::FCON);
        fcon &= ~flash::FCON_WSPFLASH_MASK;
        fcon |= (ws << 0) & flash::FCON_WSPFLASH_MASK;
        r32(flash::FCON) = fcon;
    }

    // Walks K2DIV one step at a time: a large K2DIV DECREASE (a frequency RISE) draws a current
    // step the core supply cannot service in one edge (a VDDC droop), so never jump. The PLL
    // stays locked across a K2DIV change.
    static inline void pll_k2div_staircase(uint32_t from_k2, uint32_t to_k2)
    {
        uint32_t k = from_k2;
        while (k > to_k2)
        {
            k--;
            r32(scu::PLLCON1) = pllcon1_value(k);
            clock_delay();
        }
        while (k < to_k2)
        {
            k++;
            r32(scu::PLLCON1) = pllcon1_value(k);
            clock_delay();
        }
    }

    // Returns fCPU as it lands: on fOFI with PBCLKCR at its reset /1 after either degrade
    // return, so fPERIPH equals fCPU there.
    static inline uint32_t bring_up()
    {
        // Wait-states MUST be raised BEFORE the CPU clock scales up, else flash fetches fault.
        uint32_t fcon = r32(flash::FCON);
        fcon &= ~flash::FCON_WSPFLASH_MASK;
        fcon |= flash::FCON_WSPFLASH_4CYC;
        r32(flash::FCON) = fcon;

        // Bring-up transients must not trap.
        uint32_t traps = scu::TRAP_SOSCWDGT | scu::TRAP_SVCOLCKT | scu::TRAP_UVCOLCKT;
        r32(scu::TRAPDIS) |= traps;
        r32(scu::TRAPCLR) = traps;

        r32(scu::PLLCON0) &= ~(scu::PLLCON0_VCOPWD | scu::PLLCON0_PLLPWD);

        if ((r32(scu::OSCHPCTRL) & scu::OSCHPCTRL_MODE_MASK) != 0)
        {
            uint32_t osc = r32(scu::OSCHPCTRL);
            osc &= ~(scu::OSCHPCTRL_MODE_MASK | scu::OSCHPCTRL_OSCVAL_MASK);
            osc |= scu::OSCHPCTRL_OSCVAL;
            r32(scu::OSCHPCTRL) = osc;

            r32(scu::PLLCON2) &= ~scu::PLLCON2_PINSEL;
            r32(scu::PLLCON0) &= ~scu::PLLCON0_OSCRES;

            if (not kickos::arch::wait_set(r32(scu::PLLSTAT), scu::PLLSTAT_OSC_USABLE))
            {
                return FOFI_HZ; // no usable crystal: the CPU stays on fOFI
            }
            r32(scu::TRAPDIS) &= ~scu::TRAP_SOSCWDGT;
        }

        // Lock first at a low K2DIV, then ramp.
        r32(scu::PLLCON0) |= scu::PLLCON0_VCOBYP;
        r32(scu::PLLCON0) |= scu::PLLCON0_FINDIS;
        r32(scu::PLLCON1) = pllcon1_value(12); // 288/12 = 24 MHz
        r32(scu::PLLCON0) |= scu::PLLCON0_OSCDISCDIS;
        r32(scu::PLLCON0) &= ~scu::PLLCON0_FINDIS;
        r32(scu::PLLCON0) |= scu::PLLCON0_RESLD;

        if (not kickos::arch::wait_set(r32(scu::PLLSTAT), scu::PLLSTAT_VCOLOCK))
        {
            return FOFI_HZ; // PLL never locked: the CPU stays on fOFI
        }

        r32(scu::PLLCON0) &= ~scu::PLLCON0_VCOBYP;
        (void)kickos::arch::wait_clear(r32(scu::PLLSTAT), scu::PLLSTAT_VCOBYST);
        // Re-arm the trap for the system VCO, not the USB VCO (never powered here).
        r32(scu::TRAPDIS) &= ~scu::TRAP_SVCOLCKT;

        // fSYS = fPLL, fCPU = fSYS, fPERIPH = fCPU/2.
        r32(scu::SYSCLKCR) = scu::SYSCLKCR_SYSSEL_PLL;
        r32(scu::CPUCLKCR) = 0;
        r32(scu::PBCLKCR) = scu::PBCLKCR_PBDIV_DIV2;

        // Stepped, to avoid a VDDC droop.
        r32(scu::PLLCON0) &= ~scu::PLLCON0_OSCDISCDIS;
        r32(scu::PLLCON1) = pllcon1_value(6); clock_delay(); // 288/6 = 48 MHz
        r32(scu::PLLCON1) = pllcon1_value(4); clock_delay(); // 288/4 = 72 MHz
        r32(scu::PLLCON1) = pllcon1_value(3); clock_delay(); // 288/3 = 96 MHz
        r32(scu::PLLCON1) = pllcon1_value(2); clock_delay(); // 288/2 = 144 MHz

        // fPLL stays the system clock through SLEEP, or a post-print WFI rescales the USIC baud
        // mid-shift.
        r32(scu::SLEEPCR) |= scu::SLEEPCR_SYSSEL_PLL;
        return FCPU_PLL_HZ;
    }

    // Every USIC runs at fPERIPH = fCPU / PBCLKCR.PBDIV, read live.
    static inline uint32_t fperiph_hz(uint32_t fcpu_hz)
    {
        if ((r32(scu::PBCLKCR) & scu::PBCLKCR_PBDIV_DIV2) != 0u)
        {
            return fcpu_hz / 2u;
        }
        return fcpu_hz;
    }
}

#endif
