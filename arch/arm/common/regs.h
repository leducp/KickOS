// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Core register definitions shared by the ARMv6-M and ARMv7-M backends -- the
// SCB/SysTick/NVIC subset whose offsets and bit values are identical across
// both architecture profiles (clean-room, from the ARM Architecture Reference
// Manuals). Arch-specific registers (v7-M BASEPRI band + DWT; each profile's
// SHPR priority bytes) stay in the per-arch regs.h.

#ifndef KICKOS_ARCH_ARM_COMMON_REGS_H
#define KICKOS_ARCH_ARM_COMMON_REGS_H

#include <stdint.h>

namespace kickos
{
    namespace arm
    {
        inline volatile uint32_t& reg32(uintptr_t addr)
        {
            return *reinterpret_cast<volatile uint32_t*>(addr);
        }

        // --- System Control Block ---
        constexpr uintptr_t SCB_ICSR = 0xE000ED04; // Interrupt Control and State
        constexpr uint32_t ICSR_PENDSVSET = 1u << 28;
        constexpr uint32_t ICSR_PENDSTCLR = 1u << 25; // clear a pending SysTick

        // --- SysTick ---
        constexpr uintptr_t SYST_CSR = 0xE000E010; // control/status
        constexpr uintptr_t SYST_RVR = 0xE000E014; // reload value
        constexpr uintptr_t SYST_CVR = 0xE000E018; // current value
        constexpr uint32_t SYST_CSR_ENABLE = 1u << 0;
        constexpr uint32_t SYST_CSR_TICKINT = 1u << 1;
        constexpr uint32_t SYST_CSR_CLKSOURCE = 1u << 2; // processor clock
        constexpr uint32_t SYST_RVR_MAX = 0x00FFFFFF; // 24-bit down-counter

        // --- NVIC ---
        constexpr uintptr_t NVIC_ISER0 = 0xE000E100;
        constexpr uintptr_t NVIC_ICER0 = 0xE000E180;
        constexpr uintptr_t NVIC_ISPR0 = 0xE000E200;
        constexpr uintptr_t NVIC_ICPR0 = 0xE000E280; // clear-pending (tier-1 re-arm)

        // --- PMSA MPU (identical register map on ARMv6-M and ARMv7-M) ---
        constexpr uintptr_t MPU_TYPE = 0xE000ED90; // DREGION [15:8] = # regions
        constexpr uintptr_t MPU_CTRL = 0xE000ED94;
        constexpr uintptr_t MPU_RNR = 0xE000ED98;  // region-number select
        constexpr uintptr_t MPU_RBAR = 0xE000ED9C; // region base address
        constexpr uintptr_t MPU_RASR = 0xE000EDA0; // region attr + size
        constexpr uint32_t MPU_CTRL_ENABLE = 1u << 0;
        constexpr uint32_t MPU_CTRL_PRIVDEFENA = 1u << 2; // priv uses the default map
        constexpr uint32_t MPU_RASR_ENABLE = 1u << 0;
        // RASR fields: SIZE[5:1] (region = 2^(SIZE+1)); memory-type TEX[21:19]/
        // S[18]/C[17]/B[16]; AP[26:24] (access permission); XN[28] (execute-never).
        constexpr uint32_t MPU_RASR_XN = 1u << 28;
        constexpr uint32_t MPU_RASR_AP_RW = 0x3u << 24; // priv RW, unpriv RW
        constexpr uint32_t MPU_RASR_AP_RO = 0x6u << 24; // priv RO, unpriv RO
        constexpr uint32_t MPU_RASR_MEM_NORMAL = (1u << 17) | (1u << 16); // C=1,B=1
        constexpr uint32_t MPU_RASR_MEM_DEVICE = (1u << 18) | (1u << 16); // S=1,B=1 (shared device)

        // SCB System Handler Control and State: enable the MemManage fault so an
        // MPU violation raises MemManage (not an escalated HardFault).
        constexpr uintptr_t SCB_SHCSR = 0xE000ED24;
        constexpr uint32_t SHCSR_MEMFAULTENA = 1u << 16;
    }
}

#endif
