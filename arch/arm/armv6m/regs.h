// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Minimal ARMv6-M (Cortex-M0/M0+) core register definitions -- arch-internal,
// clean-room (offsets from the ARMv6-M Architecture Reference Manual). Versus
// v7-M: NO BASEPRI (critical section is PRIMASK = mask all), NO DWT cycle
// counter (the monotonic clock is chip-provided), and SHPR/NVIC-IPR are
// word-access only (no byte writes).

#ifndef KICKOS_ARCH_ARMV6M_REGS_H
#define KICKOS_ARCH_ARMV6M_REGS_H

#include <stdint.h>

namespace kickos
{
    namespace armv6m
    {
        inline volatile uint32_t& reg32(uintptr_t addr)
        {
            return *reinterpret_cast<volatile uint32_t*>(addr);
        }

        // --- System Control Block ---
        constexpr uintptr_t SCB_ICSR = 0xE000ED04;
        constexpr uintptr_t SCB_SHPR2 = 0xE000ED1C; // SVCall (#11)
        constexpr uintptr_t SCB_SHPR3 = 0xE000ED20; // PendSV (#14) / SysTick (#15)
        constexpr uint32_t ICSR_PENDSVSET = 1u << 28;
        constexpr uint32_t ICSR_PENDSTCLR = 1u << 25;

        // --- SysTick ---
        constexpr uintptr_t SYST_CSR = 0xE000E010;
        constexpr uintptr_t SYST_RVR = 0xE000E014;
        constexpr uintptr_t SYST_CVR = 0xE000E018;
        constexpr uint32_t SYST_CSR_ENABLE = 1u << 0;
        constexpr uint32_t SYST_CSR_TICKINT = 1u << 1;
        constexpr uint32_t SYST_CSR_CLKSOURCE = 1u << 2;
        constexpr uint32_t SYST_RVR_MAX = 0x00FFFFFF;

        // --- NVIC ---
        constexpr uintptr_t NVIC_ISER0 = 0xE000E100;
        constexpr uintptr_t NVIC_ICER0 = 0xE000E180;
        constexpr uintptr_t NVIC_ISPR0 = 0xE000E200;

        // System-handler priorities (only the top 2 bits are implemented on
        // v6-M). PendSV lowest so it tail-chains after every other exception;
        // device IRQ priorities are irrelevant to the crit section (PRIMASK
        // masks all), unlike the v7-M BASEPRI band.
        constexpr uint32_t PRIO_SYSTICK = 0xC0;
        constexpr uint32_t PRIO_SVCALL = 0xC0;
        constexpr uint32_t PRIO_PENDSV = 0xC0;
    }
}

#endif
