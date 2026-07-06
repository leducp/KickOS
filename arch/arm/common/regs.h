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
    }
}

#endif
