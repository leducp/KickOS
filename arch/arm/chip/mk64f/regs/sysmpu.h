// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// MK64FN1M0 SYSMPU register map (K64 Sub-Family RM ch.19): NXP bus-master
// protection (NOT the ARM core MPU). RGD0 = supervisor background; RGD1..11 =
// per-thread user grants.

#ifndef KICKOS_ARCH_ARM_CHIP_MK64F_REGS_SYSMPU_H
#define KICKOS_ARCH_ARM_CHIP_MK64F_REGS_SYSMPU_H

#include "../mmap.h"

#include <stddef.h>
#include <stdint.h>

namespace kickos::mk64f::reg::sysmpu
{
    constexpr uintptr_t CESR = mmap::SYSMPU_BASE + 0x000u;
    constexpr uintptr_t RGD = mmap::SYSMPU_BASE + 0x400u;     // RGDn word k = RGD + n*0x10 + k*4
    constexpr uintptr_t RGDAAC0 = mmap::SYSMPU_BASE + 0x800u; // WORD2 alt view (keeps VLD)
    constexpr uintptr_t EAR0 = mmap::SYSMPU_BASE + 0x010u;    // EARn = EAR0 + n*8
    constexpr uintptr_t EDR0 = mmap::SYSMPU_BASE + 0x014u;    // EDRn = EDR0 + n*8

    constexpr uint32_t CESR_VLD = 1u << 0; // global MPU enable
    constexpr size_t RGD_COUNT = 12;
    constexpr size_t SLAVE_PORTS = 5;

    constexpr uintptr_t RGD_STRIDE = 0x10u; // bytes per descriptor
    constexpr uintptr_t RGD_WORD2 = 0x8u;   // WORD2 offset within a descriptor (clears VLD)
    constexpr uintptr_t RGD_WORD3 = 0xCu;   // WORD3 offset (VLD)
    constexpr uint32_t RGD_WORD3_VLD = 1u << 0;

    // WORD2 access fields for the core's two crossbar masters (RM 19.6.1). M0 = code
    // bus, M1 = system bus. UM = user rights, SM = supervisor rights (0b11 = "same as
    // user"): M0UM[2:0] M0SM[4:3], M1UM[8:6] M1SM[10:9].
    constexpr uint32_t WORD2_M0UM_X = 1u << 0;
    constexpr uint32_t WORD2_M0UM_W = 1u << 1;
    constexpr uint32_t WORD2_M0UM_R = 1u << 2;
    constexpr uint32_t WORD2_M0SM = 0x3u << 3;
    constexpr uint32_t WORD2_M1UM_X = 1u << 6;
    constexpr uint32_t WORD2_M1UM_W = 1u << 7;
    constexpr uint32_t WORD2_M1UM_R = 1u << 8;
    constexpr uint32_t WORD2_M1SM = 0x3u << 9;
}

#endif // KICKOS_ARCH_ARM_CHIP_MK64F_REGS_SYSMPU_H
