// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Only registers whose offsets and bit values are identical on ARMv6-M and ARMv7-M
// (clean-room, from the ARM Architecture Reference Manuals); anything profile-specific
// belongs in the per-arch regs.h.

#ifndef KICKOS_ARCH_ARM_COMMON_REGS_H
#define KICKOS_ARCH_ARM_COMMON_REGS_H

#include <stdint.h>

// Set only by a host gate (tests/unit/mpuskip) that supplies these from its own register model.
#ifndef KICKOS_ARM_REGS_FROM_SEAM
#define KICKOS_ARM_REGS_FROM_SEAM 0
#endif

namespace kickos
{
    namespace arm
    {
#if KICKOS_ARM_REGS_FROM_SEAM
        volatile uint32_t& reg32(uintptr_t addr);
        void barrier_dmb(void);
        void barrier_dsb(void);
        void barrier_isb(void);
        void wait_for_interrupt(void);
#else
        inline volatile uint32_t& reg32(uintptr_t addr)
        {
            return *reinterpret_cast<volatile uint32_t*>(addr);
        }

        inline void barrier_dmb(void)
        {
            __asm volatile("dmb" ::: "memory");
        }

        inline void barrier_dsb(void)
        {
            __asm volatile("dsb" ::: "memory");
        }

        inline void barrier_isb(void)
        {
            __asm volatile("isb" ::: "memory");
        }

        inline void wait_for_interrupt(void)
        {
            __asm volatile("wfi");
        }
#endif

        // --- System Control Block ---
        constexpr uintptr_t SCB_ICSR = 0xE000ED04;
        constexpr uintptr_t SCB_VTOR = 0xE000ED08;
        constexpr uintptr_t SCB_CPACR = 0xE000ED88;
        constexpr uint32_t ICSR_PENDSVSET = 1u << 28;
        constexpr uint32_t ICSR_PENDSTCLR = 1u << 25;
        constexpr uint32_t CPACR_CP10_CP11_FULL = 0xFu << 20;

        // --- SysTick ---
        constexpr uintptr_t SYST_CSR = 0xE000E010;
        constexpr uintptr_t SYST_RVR = 0xE000E014;
        constexpr uintptr_t SYST_CVR = 0xE000E018;
        constexpr uint32_t SYST_CSR_ENABLE = 1u << 0;
        constexpr uint32_t SYST_CSR_TICKINT = 1u << 1;
        constexpr uint32_t SYST_CSR_CLKSOURCE = 1u << 2; // processor clock
        constexpr uint32_t SYST_RVR_MAX = 0x00FFFFFF; // 24-bit down-counter

        // --- NVIC ---
        constexpr uintptr_t NVIC_ISER0 = 0xE000E100;
        constexpr uintptr_t NVIC_ICER0 = 0xE000E180;
        constexpr uintptr_t NVIC_ISPR0 = 0xE000E200;
        constexpr uintptr_t NVIC_ICPR0 = 0xE000E280;

        // --- PMSA MPU ---
        constexpr uintptr_t MPU_TYPE = 0xE000ED90; // DREGION [15:8] = # regions
        constexpr uintptr_t MPU_CTRL = 0xE000ED94;
        constexpr uintptr_t MPU_RNR = 0xE000ED98;
        constexpr uintptr_t MPU_RBAR = 0xE000ED9C;
        constexpr uintptr_t MPU_RASR = 0xE000EDA0;
        constexpr uint32_t MPU_CTRL_ENABLE = 1u << 0;
        constexpr uint32_t MPU_CTRL_PRIVDEFENA = 1u << 2; // priv uses the default map
        constexpr uint32_t MPU_RASR_ENABLE = 1u << 0;
        // RASR fields: SIZE[5:1] (region = 2^(SIZE+1)); memory-type TEX[21:19]/
        // S[18]/C[17]/B[16]; AP[26:24] (access permission); XN[28] (execute-never).
        constexpr uint32_t MPU_RASR_XN = 1u << 28;
        constexpr uint32_t MPU_RASR_AP_RW = 0x3u << 24; // priv RW, unpriv RW
        constexpr uint32_t MPU_RASR_AP_RO = 0x6u << 24; // priv RO, unpriv RO
        constexpr uint32_t MPU_RASR_AP_URO = 0x2u << 24; // priv RW, unpriv RO
        constexpr uint32_t MPU_RASR_AP_NONE = 0x0u << 24; // no access
        constexpr uint32_t MPU_RASR_AP_PRO = 0x5u << 24;  // priv RO, unpriv none
        constexpr uint32_t MPU_RASR_MEM_NORMAL = (1u << 17) | (1u << 16); // C=1,B=1
        constexpr uint32_t MPU_RASR_MEM_DEVICE = (1u << 18) | (1u << 16); // S=1,B=1 (shared device)
        // TEX=0b001, C=0, B=0, S=1 (DDI0403E Table B3-13): Normal, outer AND inner
        // non-cacheable, shareable. Not the Device type above: Device makes an unaligned
        // access UNPREDICTABLE, and this region is walked by memcpy over byte rings.
        constexpr uint32_t MPU_RASR_MEM_NORMAL_NC = (1u << 19) | (1u << 18);

        // Without MEMFAULTENA an MPU violation escalates to HardFault.
        constexpr uintptr_t SCB_SHCSR = 0xE000ED24;
        constexpr uint32_t SHCSR_MEMFAULTENA = 1u << 16;
        constexpr uint32_t SHCSR_BUSFAULTENA = 1u << 17;
    }
}

// DSB+ISB so no FP instruction is prefetched ahead of the enable. Call before any code a
// hard-float ABI could emit FP into.
static inline void kickos_armv7m_enable_fpu()
{
    kickos::arm::reg32(kickos::arm::SCB_CPACR) |= kickos::arm::CPACR_CP10_CP11_FULL;
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
}

#endif
