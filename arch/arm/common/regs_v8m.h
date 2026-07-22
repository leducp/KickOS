// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ARMv8-M (PMSAv8) MPU register definitions, clean-room from the ARMv8-M
// Architecture Reference Manual. The Cortex-M33 shares MPU_TYPE/CTRL/RNR and the
// SHCSR MEMFAULTENA bit with v7-M (regs.h), but the region descriptor changed: the
// low 5 bits of MPU_RBAR are SH/AP/XN (reserved on v7-M), 0xE000EDA0 is MPU_RLAR
// (a base+limit pair, NOT the v7-M size-encoded RASR), and memory type moves to an
// 8-slot attribute indirection through MPU_MAIR0/1. These are the pieces the shared
// v7-M regs.h cannot express; the common SCB/RNR/CTRL constants are reused from it.
//
// Addresses are the Secure MPU alias (0xE000EDxx). KickOS runs single-security-state
// Secure on the M33 (no TrustZone partition), so this governs every access; the
// Non-secure MPU alias (0xE002EDxx) is not used.

#ifndef KICKOS_ARCH_ARM_COMMON_REGS_V8M_H
#define KICKOS_ARCH_ARM_COMMON_REGS_V8M_H

#include "regs.h" // reg32, MPU_TYPE/CTRL/RNR/RBAR addrs, MPU_CTRL_*, SCB_SHCSR/MEMFAULTENA

namespace kickos
{
    namespace arm
    {
        // 0xE000ED9C (MPU_RBAR) and the CTRL/RNR/TYPE addresses come from regs.h;
        // only the RLAR/MAIR addresses and the v8-M field encodings are new here.
        constexpr uintptr_t MPU_RLAR = 0xE000EDA0;  // region limit + AttrIndx + EN
        constexpr uintptr_t MPU_MAIR0 = 0xE000EDC0; // attribute slots 0..3
        constexpr uintptr_t MPU_MAIR1 = 0xE000EDC4; // attribute slots 4..7

        // MPU_RBAR: BASE[31:5] | SH[4:3] | AP[2:1] | XN[0]. Base is 32-byte aligned,
        // so BASE occupies exactly the masked high bits.
        constexpr uint32_t RBAR_BASE_MASK = 0xFFFFFFE0u;
        constexpr uint32_t RBAR_XN = 1u << 0; // execute-never
        // AP[2:1] (ARMv8-M ARM): 00 RW priv-only, 01 RW any, 10 RO priv-only, 11 RO any.
        // "any" = unprivileged access permitted; privileged reaches everything via the
        // PRIVDEFENA background regardless, matching the v7-M AP_RW/AP_RO intent.
        constexpr uint32_t RBAR_AP_RW_ANY = 0x1u << 1;
        constexpr uint32_t RBAR_AP_RO_ANY = 0x3u << 1;
        // SH[4:3] left 0 (non-shareable): single-core, Normal memory needs no
        // cross-observer ordering. SMP (dual-M33, M5) revisits this for shared data.

        // MPU_RLAR: LIMIT[31:5] | AttrIndx[3:1] | EN[0]. LIMIT is the INCLUSIVE top
        // address (base+size-1) with its low 5 bits implied 0x1F by the hardware.
        constexpr uint32_t RLAR_LIMIT_MASK = 0xFFFFFFE0u;
        constexpr uint32_t RLAR_EN = 1u << 0;
        constexpr uint32_t RLAR_ATTR_NORMAL = 0u << 1; // AttrIndx 0 -> MAIR0 slot 0
        constexpr uint32_t RLAR_ATTR_DEVICE = 1u << 1; // AttrIndx 1 -> MAIR0 slot 1

        // MAIR attribute bytes (indexed by AttrIndx). Slot 0: Normal, outer+inner
        // Write-Back, Read/Write-Allocate (0xFF) -- cacheable XIP flash + SRAM. Slot 1:
        // Device-nGnRE (0x04) -- ordered MMIO. Programmed once into MPU_MAIR0.
        constexpr uint32_t MAIR_NORMAL_WBWA = 0xFFu;
        constexpr uint32_t MAIR_DEVICE_nGnRE = 0x04u;
    }
}

#endif // KICKOS_ARCH_ARM_COMMON_REGS_V8M_H
