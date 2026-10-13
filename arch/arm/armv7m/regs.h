// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ARMv7-M core peripheral registers, arch-internal.

#ifndef KICKOS_ARCH_ARM_ARMV7M_REGS_H
#define KICKOS_ARCH_ARM_ARMV7M_REGS_H

#include <stdint.h>

#include "../common/regs.h"

namespace kickos
{
    namespace armv7m
    {
        // --- System Control Block (arch-specific: SHPR bytes for the BASEPRI band) ---
        constexpr uintptr_t SCB_SHPR2 = 0xE000ED1C; // System Handler Priority 2 (SVCall)
        constexpr uintptr_t SCB_SHPR3 = 0xE000ED20; // System Handler Priority 3 (PendSV/SysTick)

        // --- Fault status and address (v6-M has none) ---
        constexpr uintptr_t SCB_CFSR = 0xE000ED28;
        constexpr uintptr_t SCB_HFSR = 0xE000ED2C;
        constexpr uintptr_t SCB_MMFAR = 0xE000ED34;
        constexpr uintptr_t SCB_BFAR = 0xE000ED38;

        // --- NVIC (arch-specific: byte-addressable per-line priority) ---
        constexpr uintptr_t NVIC_IPR0 = 0xE000E400;

        // --- DWT / DCB (cycle counter behind arch_trace_now) ---
        constexpr uintptr_t DWT_CTRL = 0xE0001000;
        constexpr uintptr_t DWT_CYCCNT = 0xE0001004;
        constexpr uintptr_t DCB_DEMCR = 0xE000EDFC;

        constexpr uint32_t DEMCR_TRCENA = 1u << 24;
        constexpr uint32_t DWT_CTRL_CYCCNTENA = 1u << 0;

        // The BASEPRI critical section relies on this order: lock threshold (0x20) < device band
        // (>= 0x30) < SysTick/SVCall (0xE0) < PendSV (0xF0, lowest). BASEPRI = 0x20 masks every
        // kernel exception and device IRQ.
        constexpr uint32_t PRIO_LOCK_BASEPRI = 0x20;
        constexpr uint32_t PRIO_DEVICE = 0x30;  // default device-IRQ priority: masked by the lock
        constexpr uint32_t PRIO_SYSTICK = 0xE0;
        constexpr uint32_t PRIO_SVCALL = 0xE0;
        constexpr uint32_t PRIO_PENDSV = 0xF0;
    }
}

#endif
