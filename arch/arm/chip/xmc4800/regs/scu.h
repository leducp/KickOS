// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 SCU registers: clock control, PLL, OSC, peripheral clock gates and
// resets, traps. Clean-room from the XMC4700/XMC4800 Reference Manual (V1.3,
// 2016-07). Register addresses are the sub-unit bases in mmap.h plus the
// offsets below. USIC0 and CCU40 share the same CGATCLR0/PRCLR0 gate/reset
// register pair (different bits), so both are defined here.

#ifndef KICKOS_ARCH_ARM_CHIP_XMC4800_REGS_SCU_H
#define KICKOS_ARCH_ARM_CHIP_XMC4800_REGS_SCU_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::xmc::reg::scu
{
    // TRAP sub-unit.
    constexpr uintptr_t TRAPDIS = mmap::SCU_TRAP_BASE + 0x08;
    constexpr uintptr_t TRAPCLR = mmap::SCU_TRAP_BASE + 0x0C;
    constexpr uint32_t TRAP_SOSCWDGT = 1u << 0;
    constexpr uint32_t TRAP_SVCOLCKT = 1u << 2;
    constexpr uint32_t TRAP_UVCOLCKT = 1u << 3;

    // RCU sub-unit: peripheral reset clear.
    constexpr uintptr_t PRCLR0 = mmap::SCU_RCU_BASE + 0x14;

    // CLK sub-unit.
    constexpr uintptr_t CLKSET = mmap::SCU_CLK_BASE + 0x04;
    constexpr uintptr_t SYSCLKCR = mmap::SCU_CLK_BASE + 0x0C;
    constexpr uintptr_t CPUCLKCR = mmap::SCU_CLK_BASE + 0x10;
    constexpr uintptr_t PBCLKCR = mmap::SCU_CLK_BASE + 0x14;
    constexpr uintptr_t CCUCLKCR = mmap::SCU_CLK_BASE + 0x20;
    constexpr uintptr_t SLEEPCR = mmap::SCU_CLK_BASE + 0x30;
    constexpr uintptr_t CGATCLR0 = mmap::SCU_CLK_BASE + 0x48; // peripheral clock gating clear

    // OSC sub-unit (RM): MODE[5:4]=0 external crystal; OSCVAL[19:16].
    constexpr uintptr_t OSCHPCTRL = mmap::SCU_OSC_BASE + 0x04;
    constexpr uint32_t OSCHPCTRL_MODE_MASK = 3u << 4;
    constexpr uint32_t OSCHPCTRL_OSCVAL_MASK = 15u << 16;
    constexpr uint32_t OSCHPCTRL_OSCVAL = 3u << 16; // XTAL/FOSCREF - 1 = 12e6/2.5e6 - 1

    // PLL sub-unit.
    constexpr uintptr_t PLLSTAT = mmap::SCU_PLL_BASE + 0x00;
    constexpr uintptr_t PLLCON0 = mmap::SCU_PLL_BASE + 0x04;
    constexpr uintptr_t PLLCON1 = mmap::SCU_PLL_BASE + 0x08;
    constexpr uintptr_t PLLCON2 = mmap::SCU_PLL_BASE + 0x0C;

    constexpr uint32_t PLLSTAT_VCOBYST = 1u << 0;
    constexpr uint32_t PLLSTAT_VCOLOCK = 1u << 2;
    constexpr uint32_t PLLSTAT_OSC_USABLE = (1u << 7) | (1u << 8) | (1u << 9); // PLLHV|PLLLV|PLLSP

    constexpr uint32_t PLLCON0_VCOBYP = 1u << 0;
    constexpr uint32_t PLLCON0_VCOPWD = 1u << 1;
    constexpr uint32_t PLLCON0_FINDIS = 1u << 4;
    constexpr uint32_t PLLCON0_OSCDISCDIS = 1u << 6;
    constexpr uint32_t PLLCON0_PLLPWD = 1u << 16;
    constexpr uint32_t PLLCON0_OSCRES = 1u << 17;
    constexpr uint32_t PLLCON0_RESLD = 1u << 18;

    constexpr uint32_t PLLCON2_PINSEL = 1u << 0; // 0 selects OSC_HP as PLL input

    // PLLCON1 fields, each written as (value-1) (RM PLLCON1).
    constexpr uint32_t PLLCON1_NDIV_SHIFT = 8;  // [14:8]
    constexpr uint32_t PLLCON1_K2DIV_SHIFT = 16; // [22:16]
    constexpr uint32_t PLLCON1_PDIV_SHIFT = 24; // [27:24]
    // 144 MHz profile: fVCO = 12 MHz * NDIV/PDIV = 288 MHz; fPLL = fVCO/K2DIV.
    constexpr uint32_t PLL_NDIV = 24;
    constexpr uint32_t PLL_PDIV = 1;

    // SYSCLKCR.SYSSEL(16)=1 -> fSYS from fPLL (SYSDIV[7:0]=0 -> /1).
    constexpr uint32_t SYSCLKCR_SYSSEL_PLL = 1u << 16;
    // PBCLKCR.PBDIV(0)=1 -> fPERIPH = fCPU/2.
    constexpr uint32_t PBCLKCR_PBDIV_DIV2 = 1u << 0;
    // SLEEPCR.SYSSEL(0)=1 keeps fPLL as system clock in SLEEP (reset 0 = fOFI,
    // which would rescale the USIC baud mid-shift on a WFI). RM p.11-169.
    constexpr uint32_t SLEEPCR_SYSSEL_PLL = 1u << 0;
    // SLEEPCR.CCUCR(20)=1 keeps fCCU running through SLEEP (else the CCU4 time
    // base freezes on every tickless idle).
    constexpr uint32_t SLEEPCR_CCUCR = 1u << 20;

    // CLKSET.CCUCEN(4)=1 enables fCCU generation; CCUCLKCR.CCUDIV(0)=0 -> fCCU=fSYS.
    constexpr uint32_t CLKSET_CCUCEN = 1u << 4;
    constexpr uint32_t CCUCLKCR_CCUDIV = 1u << 0;

    // Peripheral gate/reset bits in the CGATCLR0/PRCLR0 pair (RM 18.10 / SCU).
    constexpr uint32_t USIC0_GATE_BIT = 1u << 11; // USIC0 in both CGATCLR0 and PRCLR0
    constexpr uint32_t CCU40_GATE_BIT = 1u << 2;  // CCU40 clock-gate bit (CGATCLR0)
    constexpr uint32_t CCU40_RESET_BIT = 1u << 2; // CCU40 reset bit (PRCLR0)
}

#endif
