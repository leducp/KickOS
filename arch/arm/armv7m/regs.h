// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Minimal ARMv7-M core peripheral register definitions -- arch-internal, NOT
// part of the porting ABI (kept out of the installed kickos/ headers). Only the
// registers the arch backend actually touches: SCB (ICSR/SHPR/CCR), SysTick,
// NVIC (enable/pending), and DWT (cycle counter) for the tickless clock.
//
// Deliberately hand-rolled instead of pulling a vendor CMSIS pack: the surface
// is tiny and clean-room (register offsets from the ARMv7-M Architecture
// Reference Manual, not vendor headers).

#ifndef KICKOS_ARCH_ARMV7M_REGS_H
#define KICKOS_ARCH_ARMV7M_REGS_H

#include <stdint.h>

#include "../common/regs.h" // reg32, SCB_ICSR/ICSR_*, SysTick, NVIC ISER/ICER/ISPR (shared)

namespace kickos
{
    namespace armv7m
    {
        // --- System Control Block (arch-specific: SHPR bytes for the BASEPRI band) ---
        constexpr uintptr_t SCB_SHPR2 = 0xE000ED1C; // System Handler Priority 2 (SVCall)
        constexpr uintptr_t SCB_SHPR3 = 0xE000ED20; // System Handler Priority 3 (PendSV/SysTick)

        // --- NVIC (arch-specific: byte-addressable per-line priority) ---
        constexpr uintptr_t NVIC_IPR0 = 0xE000E400;

        // --- DWT / DCB (cycle counter for the monotonic clock) ---
        constexpr uintptr_t DWT_CTRL = 0xE0001000;
        constexpr uintptr_t DWT_CYCCNT = 0xE0001004;
        constexpr uintptr_t DCB_DEMCR = 0xE000EDFC;

        constexpr uint32_t DEMCR_TRCENA = 1u << 24;
        constexpr uint32_t DWT_CTRL_CYCCNTENA = 1u << 0;

        // System-handler / IRQ priority bytes (top KICKOS_NVIC_PRIO_BITS bits are
        // implemented; writing the full byte is fine, low bits read back zero).
        // Ordering that the BASEPRI critical section relies on:
        //   lock threshold (0x20) < device band (>= 0x30) < SysTick/SVCall (0xE0)
        //   < PendSV (0xF0, lowest). BASEPRI = 0x20 masks everything numerically
        //   >= 0x20, i.e. all kernel exceptions + device IRQs, leaving only a
        //   future 0x00/0x10 zero-latency band unmaskable by the kernel.
        constexpr uint32_t PRIO_LOCK_BASEPRI = 0x20;
        constexpr uint32_t PRIO_DEVICE = 0x30;  // default device-IRQ priority: masked by the lock
        constexpr uint32_t PRIO_SYSTICK = 0xE0;
        constexpr uint32_t PRIO_SVCALL = 0xE0;
        constexpr uint32_t PRIO_PENDSV = 0xF0;
    }
}

#endif
