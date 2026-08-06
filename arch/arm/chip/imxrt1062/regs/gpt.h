// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// NXP i.MX RT1062 GPT (General Purpose Timer) register map (RM ch.52), instanced
// for GPT1 = the KickOS monotonic time base. Sourced from ipg_clk_24M so the
// counter is fixed at 24 MHz and immune to any ARM-PLL retune.

#ifndef KICKOS_ARCH_ARM_CHIP_IMXRT1062_REGS_GPT_H
#define KICKOS_ARCH_ARM_CHIP_IMXRT1062_REGS_GPT_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::imxrt1062::reg::gpt
{
    constexpr uintptr_t GPT1_CR = mmap::GPT1_BASE + 0x00u;
    constexpr uintptr_t GPT1_PR = mmap::GPT1_BASE + 0x04u;
    constexpr uintptr_t GPT1_SR = mmap::GPT1_BASE + 0x08u;
    constexpr uintptr_t GPT1_IR = mmap::GPT1_BASE + 0x0Cu;
    constexpr uintptr_t GPT1_CNT = mmap::GPT1_BASE + 0x24u; // RM 52.7.1: main counter (RO)

    constexpr uint32_t CR_EN = 1u << 0;
    constexpr uint32_t CR_ENMOD = 1u << 1;      // reset counter to 0 on enable
    constexpr uint32_t CR_DBGEN = 1u << 2;      // keep counting in debug ...
    constexpr uint32_t CR_WAITEN = 1u << 3;     // ... wait ...
    constexpr uint32_t CR_DOZEEN = 1u << 4;     // ... doze ...
    constexpr uint32_t CR_STOPEN = 1u << 5;     // ... and stop mode
    constexpr uint32_t CR_CLKSRC_24M = 5u << 6; // CLKSRC=0b101: 24 MHz osc
    constexpr uint32_t CR_FRR = 1u << 9;        // free-run (roll at 0xFFFFFFFF)
    constexpr uint32_t CR_EN_24M = 1u << 10;    // enable the 24 MHz osc input
    constexpr uint32_t CR_SWR = 1u << 15;       // software reset (self-clears)

    constexpr uint32_t GPT_HZ = 24000000u;
}

#endif // KICKOS_ARCH_ARM_CHIP_IMXRT1062_REGS_GPT_H
