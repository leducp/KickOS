// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// MK64FN1M0 SYSMPU register map (K64 Sub-Family RM ch.19): NXP bus-master
// protection (NOT the ARM core MPU). RGD0 = supervisor background; RGD1..11 =
// per-thread user grants.

#ifndef KICKOS_ARCH_ARM_CHIP_MK64F_REGS_SYSMPU_H
#define KICKOS_ARCH_ARM_CHIP_MK64F_REGS_SYSMPU_H

#include <kickos/chip_mmap.h>

#include "../sysmpu_error.h"

#include <stddef.h>
#include <stdint.h>

namespace kickos::mk64f::reg::sysmpu
{
    constexpr uintptr_t CESR = mmap::SYSMPU_BASE + SYSMPU_ERROR_CESR;
    constexpr uintptr_t RGD = mmap::SYSMPU_BASE + 0x400u;     // RGDn word k = RGD + n*0x10 + k*4
    constexpr uintptr_t RGDAAC0 = mmap::SYSMPU_BASE + 0x800u; // WORD2 alt view (keeps VLD)

    constexpr uint32_t CESR_VLD = SYSMPU_ERROR_CESR_VLD; // global MPU enable
    constexpr size_t RGD_COUNT = 12;

    constexpr uintptr_t RGD_STRIDE = 0x10u; // bytes per descriptor
    constexpr uintptr_t RGD_WORD2 = 0x8u;   // WORD2 offset within a descriptor (clears VLD)
    constexpr uintptr_t RGD_WORD3 = 0xCu;   // WORD3 offset (VLD)
    constexpr uint32_t RGD_WORD3_VLD = 1u << 0;
}

#endif
