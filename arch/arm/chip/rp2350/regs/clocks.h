// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2350 CLOCKS register map (RP2350 datasheet RP-008373-DS-2, 8.1): glitchless
// clock muxes for clk_ref / clk_sys / clk_peri. Offsets are CLOCKS_BASE-relative
// (see mmap.h). SELECTED registers read back one-hot on the SRC field.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2350_REGS_CLOCKS_H
#define KICKOS_ARCH_ARM_CHIP_RP2350_REGS_CLOCKS_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::rp2350::reg::clocks
{
    constexpr uintptr_t CLK_REF_CTRL = mmap::CLOCKS_BASE + 0x30u;
    constexpr uintptr_t CLK_REF_DIV = mmap::CLOCKS_BASE + 0x34u;
    constexpr uintptr_t CLK_REF_SELECTED = mmap::CLOCKS_BASE + 0x38u;
    constexpr uintptr_t CLK_SYS_CTRL = mmap::CLOCKS_BASE + 0x3cu;
    constexpr uintptr_t CLK_SYS_SELECTED = mmap::CLOCKS_BASE + 0x44u;
    constexpr uintptr_t CLK_PERI_CTRL = mmap::CLOCKS_BASE + 0x48u;
    constexpr uintptr_t CLK_USB_CTRL = mmap::CLOCKS_BASE + 0x60u;

    // clk_ref: SRC glitchless mux; SELECTED is one-hot (1 << SRC).
    constexpr uint32_t CLK_REF_SRC_XOSC = 0x2u;
    constexpr uint32_t CLK_REF_SELECTED_XOSC = 1u << 2;

    // clk_ref divider, applied BEFORE the clk_ref output that feeds the TICKS block
    // (DS 8.1). INT is 8 bits at 23:16 and 0 encodes 256; the RP2040 field is 2 bits at
    // 9:8, so RP2040 shifts read this as zero. The register resets to INT=1 but the
    // bootrom overwrites it (DS Appendix C, A3), so it must be READ, never assumed.
    constexpr uint32_t CLK_REF_DIV_INT_SHIFT = 16u;
    constexpr uint32_t CLK_REF_DIV_INT_MASK = 0xffu << CLK_REF_DIV_INT_SHIFT;
    constexpr uint32_t CLK_REF_DIV_INT_ZERO = 256u;

    // clk_ref while it is sourced from the ROSC, as the bootrom leaves it: DS 8.3.1 holds
    // it near this by adjusting CLK_REF_DIV, so the figure is already post-divider.
    // Nominal only, the ROSC is untrimmed and randomised.
    constexpr uint32_t CLK_REF_ROSC_NOMINAL_HZ = 11000000u;

    // clk_sys glitchless mux: SRC bit0 (0=clk_ref, 1=aux); AUXSRC[7:5]=0 selects
    // clksrc_pll_sys. SELECTED is one-hot on SRC (bit0=ref, bit1=aux).
    constexpr uint32_t CLK_SYS_AUXSRC_PLL = 0x0u << 5;
    constexpr uint32_t CLK_SYS_AUXSRC_MASK = 0x7u << 5;
    constexpr uint32_t CLK_SYS_SRC_REF = 0x0u;
    constexpr uint32_t CLK_SYS_SRC_AUX = 0x1u;
    constexpr uint32_t CLK_SYS_SELECTED_REF = 1u << 0;
    constexpr uint32_t CLK_SYS_SELECTED_AUX = 1u << 1;

    // clk_peri: ENABLE(bit11) | AUXSRC[7:5]. XOSC=0x4 (12 MHz, PLL-never-locks
    // fallback); CLK_SYS=0x0 tracks clk_sys (150 MHz on PLL, the ROSC the bootrom left
    // otherwise).
    constexpr uint32_t CLK_PERI_ENABLE_XOSC = (1u << 11) | (0x4u << 5);
    constexpr uint32_t CLK_PERI_ENABLE_CLK_SYS = (1u << 11) | (0x0u << 5);

    // Aux mux only (DS 8.1.3.2): CLK_USB_SELECTED reads 1 whatever the state, so a start
    // cannot be polled. Poll ENABLED, not SELECTED, when stopping the generator.
    constexpr uint32_t CLK_USB_AUXSRC_PLL_USB = 0x0u << 5;
    constexpr uint32_t CLK_USB_ENABLE = 1u << 11;
    constexpr uint32_t CLK_USB_ENABLED = 1u << 28;
    constexpr uint32_t CLK_USB_HZ = 48000000u;
    // RP2350-E12: status crosses clk_usb -> clk_sys unsynchronised and can be lost when
    // clk_sys is not at least 10 % faster than clk_usb. Below this, USB is refused.
    constexpr uint32_t CLK_SYS_MIN_FOR_USB_HZ = 53000000u;

    // clk_sys target once on PLL_SYS (DS default max, 8.6).
    constexpr uint32_t CLK_SYS_HZ = 150000000u;
    // clk_sys while it is left where the bootrom put it, on the ROSC (DS Appendix C):
    // approximate SysTick timing if the crystal never comes up. Nominal only, DS 8.3.1
    // gives the randomised spread as 18.4 to 96 MHz.
    constexpr uint32_t ROSC_NOMINAL_HZ = 48000000u;
}

#endif
