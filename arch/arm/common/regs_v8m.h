// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ARMv8-M (PMSAv8) MPU registers. The Cortex-M33 shares MPU_TYPE/CTRL/RNR and SHCSR with v7-M
// (regs.h), but the descriptor differs: the low 5 bits of MPU_RBAR are SH/AP/XN, 0xE000EDA0 is
// MPU_RLAR (a base+limit pair, NOT the v7-M size-encoded RASR), and memory type goes through
// the MPU_MAIR0/1 attribute indirection.
//
// Addresses are the Secure MPU alias (0xE000EDxx): KickOS runs single-security-state Secure on
// the M33.

#ifndef KICKOS_ARCH_ARM_COMMON_REGS_V8M_H
#define KICKOS_ARCH_ARM_COMMON_REGS_V8M_H

#include "regs.h"

namespace kickos
{
    namespace arm
    {
        constexpr uintptr_t MPU_RLAR = 0xE000EDA0;  // region limit + AttrIndx + EN
        constexpr uintptr_t MPU_MAIR0 = 0xE000EDC0; // attribute slots 0..3
        constexpr uintptr_t MPU_MAIR1 = 0xE000EDC4; // attribute slots 4..7

        // MPU_RBAR: BASE[31:5] | SH[4:3] | AP[2:1] | XN[0].
        constexpr uint32_t RBAR_BASE_MASK = 0xFFFFFFE0u;
        constexpr uint32_t RBAR_XN = 1u << 0; // execute-never
        // AP[2:1]: 00 RW priv-only, 01 RW any, 10 RO priv-only, 11 RO any, "any" meaning
        // unprivileged access permitted. Privileged reaches everything via PRIVDEFENA.
        constexpr uint32_t RBAR_AP_RW_ANY = 0x1u << 1;
        constexpr uint32_t RBAR_AP_RO_ANY = 0x3u << 1;
        // SH[4:3] is left 0: these parts have no per-core data cache, so shareability has no
        // effect. A part with one must set it on shared data.

        // MPU_RLAR: LIMIT[31:5] | AttrIndx[3:1] | EN[0]. LIMIT is the INCLUSIVE top
        // address (base+size-1) with its low 5 bits implied 0x1F by the hardware.
        constexpr uint32_t RLAR_LIMIT_MASK = 0xFFFFFFE0u;
        constexpr uint32_t RLAR_EN = 1u << 0;
        constexpr uint32_t RLAR_ATTR_NORMAL = 0u << 1; // AttrIndx 0 -> MAIR0 slot 0
        constexpr uint32_t RLAR_ATTR_DEVICE = 1u << 1; // AttrIndx 1 -> MAIR0 slot 1
        constexpr uint32_t RLAR_ATTR_NORMAL_NC = 2u << 1; // AttrIndx 2 -> MAIR0 slot 2

        // MAIR attribute bytes, indexed by AttrIndx: Normal write-back (0xFF) for XIP flash and
        // SRAM, Device-nGnRE (0x04) for MMIO, Normal non-cacheable (0x44). In 0x44 a zero outer
        // field would make the byte Device, and an unaligned access UNPREDICTABLE.
        constexpr uint32_t MAIR_NORMAL_WBWA = 0xFFu;
        constexpr uint32_t MAIR_DEVICE_nGnRE = 0x04u;
        constexpr uint32_t MAIR_NORMAL_NC = 0x44u;
    }
}

#endif
