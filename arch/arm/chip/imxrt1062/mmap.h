// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// NXP i.MX RT1062 (Teensy 4.1) peripheral base addresses (i.MX RT1060 Processor
// Reference Manual, Rev. 3, IMXRT1060RM). Bases only; register offsets + fields
// live in regs/<periph>.h. Hand-rolled, no vendor CMSIS/SDK.

#ifndef KICKOS_ARCH_ARM_CHIP_IMXRT1062_MMAP_H
#define KICKOS_ARCH_ARM_CHIP_IMXRT1062_MMAP_H

#include <stdint.h>

namespace kickos::imxrt1062::mmap
{
    // CCM clock control + gating (RM ch.14). Owns the CCGR clock-gate roots.
    constexpr uintptr_t CCM_BASE = 0x400FC000u;

    // IOMUXC pin mux + daisy-chain input selects (RM ch.11).
    constexpr uintptr_t IOMUXC_BASE = 0x401F8000u;

    // LPUART6 = Teensy "Serial1" (pins 0/1). Base from AIPS-2 map (RM Table 3-3).
    constexpr uintptr_t LPUART6_BASE = 0x40198000u;

    // GPT1 general-purpose timer, monotonic time base (RM ch.52, Table 3-3).
    constexpr uintptr_t GPT1_BASE = 0x401EC000u;

    // Watchdogs. WDOG1/2 = 16-bit power-down watchdogs (RM ch.57); RTWDOG (WDOG3)
    // = the boot-ROM-rearmed 32-bit low-power watchdog (RM ch.58).
    constexpr uintptr_t WDOG1_BASE = 0x400B8000u;
    constexpr uintptr_t WDOG2_BASE = 0x400D0000u;
    constexpr uintptr_t RTWDOG_BASE = 0x400BC000u;
}

#endif // KICKOS_ARCH_ARM_CHIP_IMXRT1062_MMAP_H
