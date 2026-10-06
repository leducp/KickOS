// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2350 ACCESSCTRL bus access registers (RP2350 datasheet RP-008373-DS-2, 10.6). Offsets are
// ACCESSCTRL_BASE-relative (10.6.3, p.826; Table 911, pp.827-829).

#ifndef KICKOS_ARCH_ARM_CHIP_RP2350_REGS_ACCESSCTRL_H
#define KICKOS_ARCH_ARM_CHIP_RP2350_REGS_ACCESSCTRL_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::rp2350::reg::accessctrl
{
    constexpr uintptr_t BASE = mmap::ACCESSCTRL_BASE;

    // The per-peripheral registers run from ROM to XIP_AUX. Below them sit LOCK, FORCE_CORE_NS,
    // CFGRESET and the GPIO masks, none of which takes a peripheral's image.
    constexpr uint32_t FIRST_BUS_REG = 0x14u;
    constexpr uint32_t LAST_BUS_REG = 0xE8u;

    // No node is assigned these: platform/rp2350/chip.yaml's `never_assigned`.
    constexpr uint32_t NEVER_ASSIGNED[] = {
        0x14u, // ROM
        0x18u, // XIP_MAIN
        0x1Cu, 0x20u, 0x24u, 0x28u, 0x2Cu, 0x30u, 0x34u, 0x38u, 0x3Cu, 0x40u, // SRAM0 to SRAM9
        0x44u, // DMA
        0x64u, // RESETS
        0x68u, // IO_BANK0
        0x70u, // PADS_BANK0
        0x98u, // TIMER0
        0xA4u, // UART1
        0xC0u, // CLOCKS
        0xC4u, // XOSC
        0xCCu, // PLL_SYS
        0xD0u, // PLL_USB
        0xD4u, // TICKS
        0xD8u, // WATCHDOG
        0xE0u, // XIP_CTRL
        0xE4u, // XIP_QMI
        0xE8u, // XIP_AUX
    };

    // A write without it in bits 31:16 bus-faults (10.6, p.823).
    constexpr uint32_t PASSWORD = 0xACCEu << 16;

    // A manager bit admits only together with a security/privilege bit, and SU only with SP
    // (10.6.2, pp.824-825; Table 952, p.855).
    constexpr uint32_t DBG = 1u << 7;
    constexpr uint32_t DMA = 1u << 6;
    constexpr uint32_t CORE1 = 1u << 5;
    constexpr uint32_t CORE0 = 1u << 4;
    constexpr uint32_t SP = 1u << 3;
    constexpr uint32_t SU = 1u << 2;
    constexpr uint32_t NSP = 1u << 1;
    constexpr uint32_t NSU = 1u << 0;

    constexpr uint32_t CORES = 2u;

    constexpr uint32_t core_bit(uint32_t core)
    {
        return CORE0 << core;
    }
}

#endif
