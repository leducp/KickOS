// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 CCU40 registers: four 16-bit slices chained into one free-running
// 64-bit hardware counter used as the monotonic time base. Clean-room from the
// XMC4700/XMC4800 Reference Manual (V1.3, 2016-07, ch.23). The module also needs
// its SCU clock-gate/reset released and the module prescaler started (see
// regs/scu.h CCU40_GATE_BIT/CCU40_RESET_BIT + GIDLC_SPRB) before any slice runs.

#ifndef KICKOS_ARCH_ARM_CHIP_XMC4800_REGS_CCU4_H
#define KICKOS_ARCH_ARM_CHIP_XMC4800_REGS_CCU4_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::xmc::reg::ccu4
{
    // Global control (offset from CCU40_BASE).
    constexpr uintptr_t GIDLC = mmap::CCU40_BASE + 0x00C; // global idle clear
    constexpr uintptr_t GCSS = mmap::CCU40_BASE + 0x010;  // global channel set (shadow transfer)

    // Slices: SLICE0 (CC40) then +SLICE_STRIDE each.
    constexpr uintptr_t SLICE0 = mmap::CCU40_BASE + 0x100;
    constexpr uintptr_t SLICE_STRIDE = 0x100;
    constexpr uintptr_t slice(unsigned s, uintptr_t reg) { return SLICE0 + s * SLICE_STRIDE + reg; }

    // Per-slice register offsets.
    namespace slice_off
    {
        constexpr uintptr_t CMC = 0x004;   // TCE @bit20 (concatenation enable)
        constexpr uintptr_t TCSET = 0x00C; // TRBS @bit0
        constexpr uintptr_t TC = 0x014;    // counting mode (0 = edge-aligned up-count)
        constexpr uintptr_t PSC = 0x024;   // prescaler (0 = fCCU/1)
        constexpr uintptr_t PRS = 0x034;   // shadow period
        constexpr uintptr_t TIMER = 0x070; // current 16-bit slice value
    }

    constexpr uint32_t GIDLC_SPRB = 1u << 8;   // start the module prescaler
    constexpr uint32_t GIDLC_CLEAR_ALL = 0xFu; // CS0I..CS3I: clear idle, slices 0-3
    constexpr uint32_t GCSS_SHADOW_ALL =       // S0SE|S1SE|S2SE|S3SE
        (1u << 0) | (1u << 4) | (1u << 8) | (1u << 12);
    constexpr uint32_t CMC_TCE = 1u << 20;   // count the previous slice's overflow
    constexpr uint32_t TCSET_TRBS = 1u << 0; // set the slice run bit
    constexpr uint32_t PERIOD_MAX = 0xFFFFu; // wrap at 0xFFFF so each slice carries the next
}

#endif
