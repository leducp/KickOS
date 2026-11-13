// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// MCGOUTCLK from FEI to PEE at 120 MHz off the FRDM's 50 MHz external clock (K64 RM ch.25), and
// back to FEI at 20.97 MHz on any failure.

#ifndef KICKOS_ARCH_ARM_CHIP_MK64F_CLOCK_BRINGUP_H
#define KICKOS_ARCH_ARM_CHIP_MK64F_CLOCK_BRINGUP_H

#include <kickos/arch/bounded_wait.h>
#include <kickos/io/mmio.h>

#include <stdint.h>

#include "regs/mcg.h"
#include "regs/osc.h"
#include "regs/sim.h"

namespace kickos::mk64f::clocktree
{
    // FEI: the 32.768 kHz internal reference x 640 FLL.
    constexpr uint32_t FEI_HZ = 20971520u;
    constexpr uint32_t PEE_HZ = 120000000u;

    static inline bool mcg_wait(uint8_t mask, uint8_t want)
    {
        return kickos::arch::wait_bits<uint8_t>(r8(reg::mcg::S), mask, want);
    }

    // Back to the FEI reset posture: after a partial bring-up the core would run at 50/120 MHz
    // while software believes 20.97 MHz, with CLKS=EXT armed so a late reference completes the
    // switch mid-run. The landed rate is ASSUMED, not confirmed: the two waits are best-effort.
    static inline bool fail_to_fei()
    {
        r8(reg::mcg::C6) = 0;
        r8(reg::mcg::C5) = 0;
        r8(reg::mcg::C1) = reg::mcg::C1_IREFS_INT;
        mcg_wait(reg::mcg::S_IREFST, reg::mcg::S_IREFST);
        mcg_wait(reg::mcg::S_CLKST_MASK, 0);
        return false;
    }

    // FEI -> FBE -> PBE -> PEE; a failure after the external switch restores FEI. The caller
    // widens SIM_CLKDIV1 before the rise and records the landed rate.
    static inline bool mcg_to_pee()
    {
        // EREFS0=0 bypasses the crystal oscillator.
        r8(reg::osc::CR) = reg::osc::CR_ERCLKEN;
        r8(reg::mcg::C2) = reg::mcg::C2_RANGE_VHF;

        // /1536 keeps the unused FLL input inside its 31.25-39.0625 kHz window.
        r8(reg::mcg::C1) = reg::mcg::C1_CLKS_EXT | reg::mcg::C1_FRDIV_1536;
        if (not mcg_wait(reg::mcg::S_IREFST, 0))
        {
            return fail_to_fei();
        }
        if (not mcg_wait(reg::mcg::S_CLKST_MASK, reg::mcg::S_CLKST_EXT))
        {
            return fail_to_fei();
        }

        // PLL ref 2.5 MHz, VCO 120 MHz.
        r8(reg::mcg::C5) = reg::mcg::C5_PRDIV_20;
        r8(reg::mcg::C6) = reg::mcg::C6_PLLS | reg::mcg::C6_VDIV_48;
        if (not mcg_wait(reg::mcg::S_PLLST, reg::mcg::S_PLLST))
        {
            return fail_to_fei();
        }
        if (not mcg_wait(reg::mcg::S_LOCK0, reg::mcg::S_LOCK0))
        {
            return fail_to_fei();
        }

        r8(reg::mcg::C1) = reg::mcg::C1_CLKS_PLL | reg::mcg::C1_FRDIV_1536;
        if (not mcg_wait(reg::mcg::S_CLKST_MASK, reg::mcg::S_CLKST_PLL))
        {
            return fail_to_fei();
        }
        return true;
    }

    // The core clock it lands on. Dividers BEFORE the core scales up, else bus and flash overrun
    // when MCGOUTCLK jumps (RM 26).
    static inline uint32_t bring_up(uint32_t clkdiv1)
    {
        r32(reg::sim::CLKDIV1) = clkdiv1;
        if (mcg_to_pee())
        {
            return PEE_HZ;
        }
        return FEI_HZ;
    }
}

#endif
