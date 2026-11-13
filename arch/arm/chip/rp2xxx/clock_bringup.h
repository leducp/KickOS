// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The crystal, clk_ref and clk_sys on PLL_SYS, falling back to the ROSC without the crystal and to
// the crystal without the PLL, for both parts. The part's family_map.h and regs/ come from the
// chip's include path; the system timer's tick starts off clk_ref.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2XXX_CLOCK_BRINGUP_H
#define KICKOS_ARCH_ARM_CHIP_RP2XXX_CLOCK_BRINGUP_H

#include <kickos/arch/bounded_wait.h>
#include <kickos/io/mmio.h>

#include <stdint.h>

#include "bringup_values.h"
#include "family_map.h"
#include "regs/clocks.h"
#include "regs/pll.h"
#include "regs/resets.h"
#include "regs/xosc.h"

namespace kickos::rp2xxx::clocktree
{
    static inline bool wait(uintptr_t addr, uint32_t mask)
    {
        return kickos::arch::wait_set(r32(addr), mask);
    }

    // Bring PLL_SYS up to the chip's clk_sys target. Returns false, its post-divider left powered
    // down, if the VCO never locks, so the caller stays on the crystal instead of switching
    // clk_sys onto a dead PLL. Datasheet 2.18.2 / 8.6.4 sequence.
    static inline bool pll_sys_lock()
    {
        // Reset the block first so a warm reboot can't run this off stale dividers.
        r32(reg::resets::RESET + ATOMIC_SET) = reg::resets::PLL_SYS;
        r32(reg::resets::RESET + ATOMIC_CLR) = reg::resets::PLL_SYS;
        wait(reg::resets::RESET_DONE, reg::resets::PLL_SYS);

        // Load REFDIV + FBDIV BEFORE powering the VCO.
        r32(reg::pll::CS) = reg::pll::CS_REFDIV_1;
        r32(reg::pll::FBDIV_INT) = reg::pll::FBDIV_125;
        // Power up main regulator + VCO (clear PD, VCOPD). DSMPD stays set (integer FBDIV, no
        // delta-sigma); POSTDIVPD stays set until after lock.
        r32(reg::pll::PWR + ATOMIC_CLR) = reg::pll::PWR_PD | reg::pll::PWR_VCOPD;
        if (not wait(reg::pll::CS, reg::pll::CS_LOCK))
        {
            return false;
        }
        r32(reg::pll::PRIM) = reg::pll::PRIM_POSTDIV;
        r32(reg::pll::PWR + ATOMIC_CLR) = reg::pll::PWR_POSTDIVPD;
        return true;
    }

    // Returns the clk_sys it lands on.
    static inline uint32_t bring_up()
    {
        r32(reg::xosc::STARTUP) = reg::xosc::STARTUP_DELAY;
        // Program the frequency range, THEN start the oscillator (datasheet 2.16 / 8.2.7): a
        // combined write could latch ENABLE before FREQ_RANGE.
        r32(reg::xosc::CTRL) = reg::xosc::FREQ_1_15MHZ;
        r32(reg::xosc::CTRL + ATOMIC_SET) = reg::xosc::ENABLE;

        bool xosc_ok = wait(reg::xosc::STATUS, reg::xosc::STATUS_STABLE);
        if (xosc_ok)
        {
            r32(reg::clocks::CLK_REF_CTRL) = reg::clocks::CLK_REF_SRC_XOSC;
            xosc_ok = wait(reg::clocks::CLK_REF_SELECTED, reg::clocks::CLK_REF_SELECTED_XOSC);
        }

        if (not xosc_ok)
        {
            r32(reg::clocks::CLK_PERI_CTRL) = reg::clocks::CLK_PERI_ENABLE_CLK_SYS;
            part::tick_start(tick_cycles(reg::clocks::CLK_REF_ROSC_NOMINAL_HZ));
            return reg::clocks::ROSC_NOMINAL_HZ;
        }

        // clk_ref stays on the crystal: the system timer's tick derives from it and MUST NOT
        // track the PLL.
        part::tick_start(tick_cycles(part::clk_ref_xosc_hz()));

        if (pll_sys_lock())
        {
            // The glitchless clk_sys mux (datasheet 2.15.3.1 / 8.1.3.2): AUXSRC while still on
            // clk_ref, then SRC to aux, each confirmed through SELECTED.
            r32(reg::clocks::CLK_SYS_CTRL) = reg::clocks::CLK_SYS_AUXSRC_PLL | reg::clocks::CLK_SYS_SRC_REF;
            wait(reg::clocks::CLK_SYS_SELECTED, reg::clocks::CLK_SYS_SELECTED_REF);
            r32(reg::clocks::CLK_SYS_CTRL) = reg::clocks::CLK_SYS_AUXSRC_PLL | reg::clocks::CLK_SYS_SRC_AUX;
            wait(reg::clocks::CLK_SYS_SELECTED, reg::clocks::CLK_SYS_SELECTED_AUX);
            r32(reg::clocks::CLK_PERI_CTRL) = reg::clocks::CLK_PERI_ENABLE_CLK_SYS;
            return reg::clocks::CLK_SYS_HZ;
        }
        r32(reg::clocks::CLK_PERI_CTRL) = reg::clocks::CLK_PERI_ENABLE_XOSC;
        return reg::clocks::CLK_SYS_NO_PLL_HZ;
    }

    // Which of the two divisor pairs clk_peri needs. bring_up() leaves clk_peri on either
    // PLL_SYS or a slower fallback source, and CLOCKS is the hardware's own record of which.
    // The aux mux alone does not answer it: the RP2350 bootrom parks clk_sys on the ROSC
    // through that same mux, so the aux SOURCE has to be PLL_SYS too.
    static inline bool console_clk_on_pll()
    {
        bool const on_aux =
            (r32(reg::clocks::CLK_SYS_SELECTED) & reg::clocks::CLK_SYS_SELECTED_AUX) != 0;
        bool const aux_is_pll =
            (r32(reg::clocks::CLK_SYS_CTRL) & reg::clocks::CLK_SYS_AUXSRC_MASK) ==
            reg::clocks::CLK_SYS_AUXSRC_PLL;
        return on_aux and aux_is_pll;
    }
}

#endif
