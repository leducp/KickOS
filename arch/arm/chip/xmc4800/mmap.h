// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 peripheral base addresses. Clean-room from the XMC4700/XMC4800
// Reference Manual (V1.3, 2016-07); no XMCLib/DAVE/CMSIS vendor source. Bases
// only; per-peripheral register offsets/bit fields live in regs/<periph>.h.

#ifndef KICKOS_ARCH_ARM_CHIP_XMC4800_MMAP_H
#define KICKOS_ARCH_ARM_CHIP_XMC4800_MMAP_H

#include <stdint.h>

namespace kickos::xmc::mmap
{
    // USIC channels (RM Table 18-21). Each module holds two 0x200 channels; a
    // second channel or module is just another base plus its own SCU gate/reset
    // bit. USIC0 sits in the 0x4003_0000 island; USIC1/USIC2 in 0x4802_xxxx.
    constexpr uintptr_t USIC0_CH0_BASE = 0x40030000;
    constexpr uintptr_t USIC0_CH1_BASE = 0x40030200;
    constexpr uintptr_t USIC1_CH0_BASE = 0x48020000;
    constexpr uintptr_t USIC2_CH0_BASE = 0x48024000;
    constexpr uintptr_t USIC_CHANNEL_STRIDE = 0x200; // CH0 -> CH1 within a module
    constexpr uintptr_t USIC_MODULE_SPAN = 0x400;    // two channels of 0x200

    // Ports P0..P15 (RM ch.26). Pn base = PORT0_BASE + n*PORT_STRIDE.
    constexpr uintptr_t PORT0_BASE = 0x48028000;
    constexpr uintptr_t PORT_STRIDE = 0x100;

    // SCU (RM SCU chapter). One 0x1000 block at SCU_BASE; the sub-units below are
    // absolute region bases within it (each offset is added in regs/scu.h).
    constexpr uintptr_t SCU_BASE = 0x50004000;
    constexpr uintptr_t SCU_TRAP_BASE = 0x50004160;
    constexpr uintptr_t SCU_RCU_BASE = 0x50004400; // peripheral reset (PRCLR/PRSET)
    constexpr uintptr_t SCU_CLK_BASE = 0x50004600; // clock control + gates (CGATCLR/CGATSET)
    constexpr uintptr_t SCU_OSC_BASE = 0x50004700;
    constexpr uintptr_t SCU_PLL_BASE = 0x50004710;

    // CCU40 (RM ch.23): the monotonic time base (four chained 16-bit slices).
    constexpr uintptr_t CCU40_BASE = 0x4000C000;

    // Flash: the FLASH0 controller (FCON etc.) vs the cached code/exec alias the
    // image is linked at and VTOR points to. These are two different addresses
    // for the same flash: the alias is memory, the controller is registers.
    constexpr uintptr_t FLASH0_CTRL_BASE = 0x58001000;
    constexpr uintptr_t FLASH_CACHED_BASE = 0x08000000;
}

#endif
