// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// NXP i.MX RT1062 (Teensy 4.1) peripheral base addresses (i.MX RT1060 Processor
// Reference Manual, Rev. 3, IMXRT1060RM). Bases only; register offsets + fields
// live in regs/<periph>.h. Hand-rolled, no vendor CMSIS/SDK.

#ifndef KICKOS_CHIP_MMAP_H
#define KICKOS_CHIP_MMAP_H

#include <stdint.h>

namespace kickos::imxrt1062::mmap
{
    // CCM clock control + gating (RM ch.14). Owns the CCGR clock-gate roots.
    constexpr uintptr_t CCM_BASE = 0x400FC000u;

    // IOMUXC pin mux + daisy-chain input selects (RM ch.11).
    constexpr uintptr_t IOMUXC_BASE = 0x401F8000u;

    // IOMUXC general-purpose registers (RM 11.3), a SEPARATE block from the pad mux above.
    // GPR27 is the one this chip needs: it picks GPIO2 or GPIO7 per pad.
    constexpr uintptr_t IOMUXC_GPR_BASE = 0x400AC000u;

    // GPIO2 (RM ch.12). Carries the board's diagnostic LED on IO03. GPIO7 at 4200_4000
    // shares the same pads and is NOT used here; see GPR27 in regs/iomuxc.h.
    constexpr uintptr_t GPIO2_BASE = 0x401BC000u;

    // LPUART6 = Teensy "Serial1" (pins 0/1). Base from AIPS-2 map (RM Table 3-3).
    constexpr uintptr_t LPUART6_BASE = 0x40198000u;

    // GPT1 general-purpose timer, monotonic time base (RM ch.52, Table 3-3).
    constexpr uintptr_t GPT1_BASE = 0x401EC000u;

    // CCM_ANALOG: the PLLs, including PLL_USB1 at +0x10 (RM 14.8). A different block from
    // CCM_BASE above, which holds only the CCGR gate roots.
    constexpr uintptr_t CCM_ANALOG_BASE = 0x400D8000u;

    // USB1 (OTG1). The core the unprivileged driver is granted is the first 512 B of this
    // (RM 42.7); USBNC below is the non-core control at +0x800 and stays the kernel's.
    constexpr uintptr_t USB1_BASE = 0x402E0000u;
    constexpr uintptr_t USBNC_BASE = 0x402E0800u;

    // USB1 UTMI PHY (RM ch.43). Kernel-only.
    constexpr uintptr_t USBPHY1_BASE = 0x400D9000u;

    // The four AIPSTZ bridge configuration blocks (RM ch.32): MPR picks which masters are
    // trusted, OPACR0..4 carry one Supervisor-Protect nibble per 16 KiB peripheral slot.
    constexpr uintptr_t AIPSTZ1_BASE = 0x4007C000u;
    constexpr uintptr_t AIPSTZ2_BASE = 0x4017C000u;
    constexpr uintptr_t AIPSTZ3_BASE = 0x4027C000u;
    constexpr uintptr_t AIPSTZ4_BASE = 0x4037C000u;

    // Watchdogs. WDOG1/2 = 16-bit power-down watchdogs (RM ch.57); RTWDOG (WDOG3)
    // = the boot-ROM-rearmed 32-bit low-power watchdog (RM ch.58).
    constexpr uintptr_t WDOG1_BASE = 0x400B8000u;
    constexpr uintptr_t WDOG2_BASE = 0x400D0000u;
    constexpr uintptr_t RTWDOG_BASE = 0x400BC000u;
}

#endif
