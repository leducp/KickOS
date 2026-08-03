// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RX72M clock-generation + SYSTEM/low-power register offsets + fields. From the
// RX72M Group User's Manual: Hardware (r01uh0804ej0120, Rev.1.20) sec.9 (CGC),
// sec.11 (module stop), sec.13 (register protect); hand-rolled, clean-room.
// Bases: mmap.h.

#ifndef KICKOS_ARCH_RX_CHIP_RX72M_REGS_CGC_H
#define KICKOS_ARCH_RX_CHIP_RX72M_REGS_CGC_H

#include <stdint.h>

#include "../mmap.h"

namespace kickos::rx::reg::cgc
{
    // --- Register protection (UM sec.13) ---
    constexpr uintptr_t PRCR = mmap::SYSTEM + 0x03FE; // 16-bit protect register
    constexpr uint16_t PRCR_UNLOCK = 0xA50B;          // key 0xA5 + PRC0|PRC1|PRC3
    constexpr uint16_t PRCR_LOCK = 0xA500;            // key 0xA5, all protect bits 0

    // --- Code-flash wait control (UM sec.9.2) ---
    constexpr uintptr_t MEMWAIT = mmap::SYSTEM + 0x101C; // 8-bit
    constexpr uint8_t MEMWAIT_ONE_WAIT = 0x01;           // one wait (required >120 MHz)

    // --- Module stop (UM sec.11) ---
    constexpr uintptr_t MSTPCRA = mmap::SYSTEM + 0x0010; // 32-bit module stop A
    constexpr uintptr_t MSTPCRB = mmap::SYSTEM + 0x0014; // 32-bit module stop B
    constexpr uint32_t MSTPA_CMTW1 = 1u << 0;  // MSTPCRA b0 = CMTW unit 1
    constexpr uint32_t MSTPA_CMTW0 = 1u << 1;  // MSTPCRA b1 = CMTW unit 0
    constexpr uint32_t MSTPB_SCI6 = 1u << 25;  // MSTPCRB b25 = SCI6

    // --- Clock generation circuit (UM sec.9), all PRC0-protected ---
    constexpr uintptr_t SCKCR = mmap::SYSTEM + 0x0020;    // 32-bit system clock control (sec.9.2.1)
    constexpr uintptr_t SCKCR3 = mmap::SYSTEM + 0x0026;   // 16-bit clock source select (sec.9.2.4)
    constexpr uintptr_t PLLCR = mmap::SYSTEM + 0x0028;    // 16-bit PLL control (sec.9.2.5)
    constexpr uintptr_t PLLCR2 = mmap::SYSTEM + 0x002A;   // 8-bit PLL stop control (sec.9.2.6)
    constexpr uintptr_t MOSCCR = mmap::SYSTEM + 0x0032;   // 8-bit main osc control (sec.9.2.8)
    constexpr uintptr_t OSCOVFSR = mmap::SYSTEM + 0x003C; // 8-bit stabilization flags (sec.9.2.14)
    constexpr uintptr_t MOSCWTCR = mmap::SYSTEM + 0x00A2; // 8-bit main osc wait control (sec.9.2.17)
    // MOFCR (main osc forced osc/drive, sec.9.2.19) is OUTSIDE the SYSTEM block.
    constexpr uintptr_t MOFCR = 0x0008C293; // 8-bit

    constexpr uint8_t OSCOVFSR_MOOVF = 1u << 0; // main clock oscillation stabilized
    constexpr uint8_t OSCOVFSR_PLOVF = 1u << 2; // PLL clock oscillation stabilized

    // MODRV2[1:0]=00 selects the 20.1..24 MHz crystal drive range (sec.9.2.19);
    // the board crystal is 24 MHz. MOFXIN=0 (no forced oscillation).
    constexpr uint8_t MOFCR_XTAL_24MHZ = 0x00;
    // MSTS wait count off the LOCO: MSTS > (tMAINOSC * fLOCO_max + 16)/32
    // (sec.9.2.17). 0x53 covers ~10 ms of crystal settling; read via OSCOVFSR.MOOVF.
    constexpr uint8_t MOSCWTCR_MSTS = 0x53;
    // PLLCR: PLIDIV=00 (/1 -> 24 MHz input), PLLSRCSEL=0 (main osc),
    // STC[5:0]=010011b (x10.0) -> 240 MHz (fPLL max). (sec.9.2.5)
    constexpr uint16_t PLLCR_PLL_240MHZ = 0x1300;
    constexpr uint8_t PLLCR2_PLL_RUN = 0x00;  // PLLEN=0 => PLL operates
    constexpr uint8_t MOSCCR_MAIN_RUN = 0x00; // MOSTP=0 => main osc operates
    // SCKCR: FCK/4(60) ICK/1(240) BCK/4(60) PCKA/2(120) PCKB/4(60) PCKC/4(60)
    // PCKD/4(60). Division field 0000=/1, 0001=/2, 0010=/4. (sec.9.2.1)
    constexpr uint32_t SCKCR_240MHZ = 0x20021222u;
    constexpr uint16_t SCKCR3_CKSEL_PLL = 0x0400; // CKSEL[2:0]=100 => PLL (sec.9.2.4)

    // Achieved-by-construction frequencies once the PLL is the system clock source.
    constexpr uint32_t ICLK_HZ = 240000000u;     // ICLK
    constexpr uint32_t PCLKB_DIV8_HZ = 7500000u; // PCLKB(60 MHz)/8 = CMTW input
}

#endif
