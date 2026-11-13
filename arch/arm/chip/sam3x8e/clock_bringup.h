// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// MCK from the 4 MHz fast RC onto the 12 MHz crystal, then PLLA/2 at 84 MHz (datasheet sec.27,
// sec.28). Every failure stays on the last selection that was confirmed.

#ifndef KICKOS_ARCH_ARM_CHIP_SAM3X8E_CLOCK_BRINGUP_H
#define KICKOS_ARCH_ARM_CHIP_SAM3X8E_CLOCK_BRINGUP_H

#include <kickos/chip_mmap.h>
#include <kickos/io/mmio.h>

#include <stdint.h>

namespace kickos::sam3x8e::clocktree
{
    constexpr uint32_t MAINCK_RC_HZ = 4000000u;
    constexpr uint32_t MAINCK_XTAL_HZ = 12000000u;

    // EEFC (sec.18): the two flash banks. FWS=4 covers up to 90 MHz (sec.45) and is required for
    // 84 MHz; set BEFORE the clock is raised.
    constexpr uintptr_t EEFC0_FMR = mmap::EEFC0_BASE + 0x00;
    constexpr uintptr_t EEFC1_FMR = mmap::EEFC1_BASE + 0x00;
    constexpr uint32_t FMR_FWS_4 = 4u << 8;

    constexpr uintptr_t CKGR_MOR = mmap::PMC_BASE + 0x20;
    constexpr uintptr_t CKGR_PLLAR = mmap::PMC_BASE + 0x28;
    constexpr uintptr_t PMC_MCKR = mmap::PMC_BASE + 0x30;
    constexpr uintptr_t PMC_SR = mmap::PMC_BASE + 0x68;

    // CKGR_MOR (sec.28): KEY 0x37 gates the write; MOSCXTST is the crystal startup counter in
    // SLCK/8; the fast RC (MOSCRCEN) stays on while the crystal warms up, then MOSCSEL picks the
    // crystal.
    //
    // MOSCXTS asserts when the MOSCXTST counter expires, NOT on physical crystal detection, so
    // MOSCXTST MUST cover the crystal's worst-case warm-up or PLLA locks on a still-settling
    // MAINCK (the intermittent-boot race). SLCK is the slow RC, 22..42 kHz, so the count is sized
    // at the FAST end: 0x80 gives 24.4 ms at 42 kHz and 46.5 ms at 22 kHz, past the ~15 ms
    // crystal spec. Unconfirmed on silicon: do not trim toward 15 ms, 0x50 has no margin at
    // 42 kHz.
    constexpr uint32_t MOR_KEY = 0x37u << 16;
    constexpr uint32_t MOR_MOSCXTEN = 1u << 0;
    constexpr uint32_t MOR_MOSCRCEN = 1u << 3;
    constexpr uint32_t MOR_MOSCXTST = 0x80u << 8;
    constexpr uint32_t MOR_MOSCSEL = 1u << 24;
    constexpr uint32_t MOR_CRYSTAL = MOR_KEY | MOR_MOSCXTST | MOR_MOSCRCEN | MOR_MOSCXTEN;

    // PLLA = MAINCK * (MULA+1) / DIVA. ONE (bit 29) reads 1; PLLCOUNT is the LOCK delay in SLCK.
    constexpr uint32_t PLLA_MUL = 14u;
    constexpr uint32_t PLLA_DIV = 1u;
    constexpr uint32_t PLLA_HZ = MAINCK_XTAL_HZ * PLLA_MUL / PLLA_DIV;
    constexpr uint32_t PLLAR_ONE = 1u << 29;
    constexpr uint32_t PLLAR_MULA = (PLLA_MUL - 1u) << 16;
    constexpr uint32_t PLLAR_COUNT = 0x3Fu << 8;
    constexpr uint32_t PLLAR_DIVA = PLLA_DIV << 0;

    constexpr uint32_t MCKR_CSS_MASK = 3u << 0;
    constexpr uint32_t MCKR_CSS_MAIN = 1u << 0;
    constexpr uint32_t MCKR_CSS_PLLA = 2u << 0;
    constexpr uint32_t MCKR_PRES_MASK = 7u << 4;
    constexpr uint32_t MCKR_PRES_DIV2 = 1u << 4;

    constexpr uint32_t MCKR_MAIN = MCKR_CSS_MAIN;
    constexpr uint32_t MCKR_MAIN_DIV2 = MCKR_PRES_DIV2 | MCKR_CSS_MAIN;
    constexpr uint32_t MCKR_PLLA_DIV2 = MCKR_PRES_DIV2 | MCKR_CSS_PLLA;

    // PRES DIVIDES WHATEVER CSS SELECTED, so PRES_DIV2 beside CSS_MAIN is the crystal halved,
    // not PLLA halved. A CSS this backend never writes answers 0, which a divisor cannot
    // mistake for a working clock.
    constexpr uint32_t mck_for(uint32_t mckr, uint32_t mainck)
    {
        uint32_t src = 0u;
        if ((mckr & MCKR_CSS_MASK) == MCKR_CSS_MAIN)
        {
            src = mainck;
        }
        else if ((mckr & MCKR_CSS_MASK) == MCKR_CSS_PLLA)
        {
            src = PLLA_HZ;
        }
        uint32_t div = 1u;
        if ((mckr & MCKR_PRES_MASK) == MCKR_PRES_DIV2)
        {
            div = 2u;
        }
        return src / div;
    }

    // The part's own maximum, and the bound FWS=4 was chosen against.
    constexpr uint32_t MCK_MAX_HZ = 84000000u;

    static_assert(mck_for(MCKR_MAIN, MAINCK_XTAL_HZ) == MAINCK_XTAL_HZ,
                  "CSS_MAIN with no PRES is MAINCK undivided");
    static_assert(mck_for(MCKR_MAIN_DIV2, MAINCK_XTAL_HZ) == 6000000u,
                  "PRES_DIV2 beside CSS_MAIN halves the CRYSTAL: the intermediate state of "
                  "the PLL switch is 6 MHz, not the 12 MHz the crystal runs at");
    static_assert(mck_for(MCKR_PLLA_DIV2, MAINCK_XTAL_HZ) == MCK_MAX_HZ,
                  "PLLA over the MCKR prescaler must land on the SAM3X8E maximum MCK; the "
                  "flash wait states are sized for that bound too");

    // The only writer of PMC_MCKR: returns the MCK that selection runs.
    static inline uint32_t mckr_select(uint32_t mckr, uint32_t mainck)
    {
        r32(PMC_MCKR) = mckr;
        return mck_for(mckr, mainck);
    }

    constexpr uint32_t SR_MOSCXTS = 1u << 0;
    constexpr uint32_t SR_LOCKA = 1u << 1;
    constexpr uint32_t SR_MCKRDY = 1u << 3;
    constexpr uint32_t SR_MOSCSELS = 1u << 16;

    // ~1M spins on the reset 4 MHz RC is hundreds of ms, well past the MOSCXTST window and
    // every SLCK-counted delay. On false the source never came up: the caller MUST NOT
    // select it (that is the boot race).
    static inline bool pmc_wait(uint32_t bit)
    {
        for (uint32_t i = 0; i < 0x100000u; i++)
        {
            if ((r32(PMC_SR) & bit) != 0)
            {
                return true;
            }
        }
        return false;
    }

    // Returns the MCK it lands on. NO DEGRADE RETURN CARRIES A RATE OF ITS OWN: the rate is
    // restated at every edge that moves MCK, and every failure returns it. PMC_MCKR resets to
    // MAINCK undivided (sec.28.15.11), so step 3's MOSCSEL moves MCK with no PMC_MCKR write of its
    // own.
    static inline uint32_t bring_up()
    {
        uint32_t mck = MAINCK_RC_HZ;
        // 1. Flash wait states first, both banks; FWS=4 also covers the fallbacks.
        r32(EEFC0_FMR) = FMR_FWS_4;
        r32(EEFC1_FMR) = FMR_FWS_4;

        // 2. Start the 12 MHz crystal; the RC stays MAINCK meanwhile. No MOSCXTS: stay on the
        //    4 MHz RC, where the console cannot reach 115200 but the core and LED still run.
        r32(CKGR_MOR) = MOR_CRYSTAL;
        if (not pmc_wait(SR_MOSCXTS))
        {
            return mck;
        }

        // 3. Select the crystal as MAINCK, then run MCK off it before touching PLLA. The
        //    MOSCSEL switch is glitch-free and signals on MOSCSELS, not MCKRDY (sec.27.5.3),
        //    and CSS already names MAINCK, so MCK is on the crystal before the write below.
        r32(CKGR_MOR) = MOR_CRYSTAL | MOR_MOSCSEL;
        if (not pmc_wait(SR_MOSCSELS))
        {
            // MOSCSEL moves MCK with no PMC_MCKR write, so a switch that landed without
            // reporting would leave MCK on the crystal while the rate below still reads the
            // RC. Deselect before returning, and the asserted rate is true either way.
            r32(CKGR_MOR) = MOR_CRYSTAL;
            return mck;
        }
        mck = mckr_select(MCKR_MAIN, MAINCK_XTAL_HZ);
        if (not pmc_wait(SR_MCKRDY))
        {
            return mck;
        }

        // 4. PLLA. If it never locks, MCK is already stable on the crystal, so stay there.
        r32(CKGR_PLLAR) = PLLAR_ONE | PLLAR_MULA | PLLAR_COUNT | PLLAR_DIVA;
        if (not pmc_wait(SR_LOCKA))
        {
            return mck;
        }

        // 5. Switch MCK to PLLA/2. A PLL source needs PRES set, MCKRDY, then CSS set, MCKRDY
        //    (sec.28.12). THE FIRST WRITE HALVES MAINCK, so the intermediate state is 6 MHz.
        mck = mckr_select(MCKR_MAIN_DIV2, MAINCK_XTAL_HZ);
        if (not pmc_wait(SR_MCKRDY))
        {
            return mck;
        }
        mck = mckr_select(MCKR_PLLA_DIV2, MAINCK_XTAL_HZ);
        (void)pmc_wait(SR_MCKRDY);
        return mck;
    }
}

#endif
