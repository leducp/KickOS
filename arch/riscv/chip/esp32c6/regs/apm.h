// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 access-permission (HP_APM) + HP_TEE registers (TRM v1.2 ch.16). The
// bus-side, per-security-mode permission unit that sits IN SERIES with PMP on this
// core (TRM 16.1): a U-mode (REE) access to an HP peripheral is checked by PMP first
// (CPU-side, per-hart) then APM (bus-side, per security mode). The default APM
// posture DENIES all REE access to every peripheral (region 0 catch-all, TRM
// 16.3.2 Note), so a U-mode driver reaches nothing until a one-time TEE-mode APM open.
//
// APM QUIRKS worth flagging:
//  - An APM denial does NOT trap (TRM 16.5): a read returns 0, a write is dropped,
//    and a separate HP_APM interrupt fires. Only PMP produces a load/store fault. So
//    per-thread isolation proofs must ride the PMP fault, never APM.
//  - HP_TEE_M0_MODE_CTRL resets to 0 => the HP CPU (master M0) runs U-mode as
//    security mode REE0; KickOS relies on that reset default (no write needed).
//  - Region regs stride 0xC: START @0x04+0xC*n, END @0x08+0xC*n, ATTR @0x0C+0xC*n.
//    ATTR REE0 permission bits are R0_X=b0, R0_W=b1, R0_R=b2.

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_APM_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_APM_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::esp32c6::reg::apm
{
    // --- HP_TEE mode controller. TEE_Mn_MODE_CTRL_REG = 0x00 + 0x4*n (Reg 16.53);
    //     HP CPU is master M0 (TRM 16.3.1). Reset 0 => U-mode security mode is REE0.
    constexpr uintptr_t HP_TEE_M0_MODE_CTRL = mmap::HP_TEE_BASE + 0x00u;

    // --- HP_APM region controller (TRM Reg 16.1-16.4).
    constexpr uintptr_t FILTER_EN = mmap::HP_APM_BASE + 0x00u; // reset 0x01: region 0 on
    constexpr uintptr_t REGION_STRIDE = 0x0Cu;

    inline constexpr uintptr_t region_addr_start(uint32_t n)
    {
        return mmap::HP_APM_BASE + 0x04u + REGION_STRIDE * n;
    }
    inline constexpr uintptr_t region_addr_end(uint32_t n) // reset 0xFFFFFFFF for region 0
    {
        return mmap::HP_APM_BASE + 0x08u + REGION_STRIDE * n;
    }
    inline constexpr uintptr_t region_attr(uint32_t n)
    {
        return mmap::HP_APM_BASE + 0x0Cu + REGION_STRIDE * n;
    }

    // FILTER_EN per-region enable bit.
    inline constexpr uint32_t region_en(uint32_t n)
    {
        return 1u << n;
    }

    // ATTR REE0 permission bits.
    constexpr uint32_t R0_X = 1u << 0;
    constexpr uint32_t R0_W = 1u << 1;
    constexpr uint32_t R0_R = 1u << 2;
}

#endif
