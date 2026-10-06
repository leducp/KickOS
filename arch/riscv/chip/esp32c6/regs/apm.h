// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 access-permission (HP_APM, LP_APM) and TEE registers (TRM v1.2 ch.16). The
// bus-side, per-security-mode permission unit that sits IN SERIES with PMP on this
// core (TRM 16.1): a U-mode (REE) access to an HP peripheral is checked by PMP first
// (CPU-side, per-hart) then APM (bus-side, per security mode). The default APM
// posture DENIES all REE access to every peripheral (region 0 catch-all, TRM
// 16.3.2 Note), so a U-mode driver reaches nothing until node 0 programs the gate.
//
// APM QUIRKS worth flagging:
//  - An APM denial does NOT trap (TRM 16.5): a read returns 0, a write is dropped,
//    and a separate HP_APM interrupt fires. Only PMP produces a load/store fault. So
//    per-thread isolation proofs must ride the PMP fault, never APM.
//  - HP_TEE_M0_MODE_CTRL resets to 0 => the HP CPU (master M0) runs U-mode as
//    security mode REE0; KickOS relies on that reset default (no write needed).
//  - Permissions are per security mode, never per master: every bus master but the two
//    CPUs is REE2 at power-up (TRM 16.3.2.3 Note), so a REE2 permit opens to DMA too.

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_APM_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_APM_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::esp32c6::reg::apm
{
    // --- HP_TEE mode controller. TEE_Mn_MODE_CTRL_REG = 0x00 + 0x4*n (Reg 16.53);
    //     HP CPU is master M0 (TRM 16.3.1). Reset 0 => U-mode security mode is REE0.
    constexpr uintptr_t HP_TEE_M0_MODE_CTRL = mmap::HP_TEE_BASE + 0x00u;

    // --- The LP CPU's mode (Reg 16.56, reset 3).
    constexpr uintptr_t LP_TEE_M0_MODE_CTRL = mmap::LP_TEE_BASE + 0x00u;

    // LP_TEE_M0_MODE values (Reg 16.56).
    constexpr uint32_t MODE_REE0 = 1u;
    constexpr uint32_t MODE_REE1 = 2u;
    constexpr uint32_t MODE_REE2 = 3u;

    // --- Region controllers. HP_APM (Reg 16.1-16.4) and LP_APM (Reg 16.25-16.28) share
    //     one layout; FILTER_EN resets to 0x01 (region 0 on), region 0 END to 0xFFFFFFFF.
    constexpr uintptr_t HP_APM_BASE = mmap::HP_APM_BASE;
    constexpr uintptr_t LP_APM_BASE = mmap::LP_APM_BASE;
    constexpr uintptr_t REGION_STRIDE = 0x0Cu;

    inline constexpr uintptr_t filter_en(uintptr_t gate)
    {
        return gate + 0x00u;
    }
    inline constexpr uintptr_t region_start(uintptr_t gate, uint32_t n)
    {
        return gate + 0x04u + REGION_STRIDE * n;
    }
    inline constexpr uintptr_t region_end(uintptr_t gate, uint32_t n) // inclusive
    {
        return gate + 0x08u + REGION_STRIDE * n;
    }
    inline constexpr uintptr_t region_attr_of(uintptr_t gate, uint32_t n)
    {
        return gate + 0x0Cu + REGION_STRIDE * n;
    }

    constexpr uintptr_t FILTER_EN = filter_en(HP_APM_BASE);

    inline constexpr uintptr_t region_addr_start(uint32_t n)
    {
        return region_start(HP_APM_BASE, n);
    }
    inline constexpr uintptr_t region_addr_end(uint32_t n)
    {
        return region_end(HP_APM_BASE, n);
    }
    inline constexpr uintptr_t region_attr(uint32_t n)
    {
        return region_attr_of(HP_APM_BASE, n);
    }

    inline constexpr uint32_t region_en(uint32_t n)
    {
        return 1u << n;
    }

    // ATTR: each REE mode owns X, W, R at bits 0, 1, 2 of its own nibble, REE0 the nibble at
    // bit 0, REE1 at bit 4, REE2 at bit 8.
    constexpr uint32_t ATTR_X = 1u << 0;
    constexpr uint32_t ATTR_W = 1u << 1;
    constexpr uint32_t ATTR_R = 1u << 2;
    constexpr uint32_t ATTR_MODE_MASK = ATTR_X | ATTR_W | ATTR_R;

    inline constexpr uint32_t attr_shift(uint32_t mode)
    {
        return 4u * (mode - MODE_REE0);
    }
}

#endif
