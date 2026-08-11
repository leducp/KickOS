// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RX72M clock-generation + SYSTEM/low-power register offsets + fields. Clean-room from the
// RX72M Group User's Manual: Hardware (r01uh0804ej0120, Rev.1.20) sec.9 (CGC), sec.11
// (module stop), sec.13 (register protect). Bases: mmap.h.

#ifndef KICKOS_ARCH_RX_CHIP_RX72M_REGS_CGC_H
#define KICKOS_ARCH_RX_CHIP_RX72M_REGS_CGC_H

#include <stdint.h>

#include <kickos/chip_mmap.h>

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
    constexpr uintptr_t HOCOCR2 = mmap::SYSTEM + 0x0037;  // 8-bit HOCO frequency select (sec.9.2.13)
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

    // --- Fields the live clock tree is READ BACK through (arch_periph_clock_hz) ---

    // SCKCR division fields (sec.9.2.1 p.349): one 4-bit selector per branch, all with the
    // same encoding 0000=/1 .. 0110=/64, i.e. divisor = 1 << field. 0111..1111 are
    // PROHIBITED, so a field above SCKCR_DIV_MAX is a tree this cannot describe. BCK has
    // one extra code (1001=/3) and is therefore NOT covered by SCKCR_DIV_MAX.
    constexpr unsigned SCKCR_PCKD_S = 0;
    constexpr unsigned SCKCR_PCKC_S = 4;
    constexpr unsigned SCKCR_PCKB_S = 8;
    constexpr unsigned SCKCR_PCKA_S = 12;
    constexpr unsigned SCKCR_ICK_S = 24;
    constexpr unsigned SCKCR_FCK_S = 28;
    constexpr uint32_t SCKCR_DIV_MASK = 0xFu;
    constexpr uint32_t SCKCR_DIV_MAX = 6u;

    // SCKCR3.CKSEL[2:0] (sec.9.2.4 p.354), the clock the SCKCR dividers are fed from.
    // 101..111 are prohibited. Reset 000, so LOCO is what a chip that never ran a clock
    // bring-up is executing on.
    constexpr unsigned SCKCR3_CKSEL_S = 8;
    constexpr uint16_t SCKCR3_CKSEL_MASK = 0x7u;
    constexpr uint16_t CKSEL_LOCO = 0;
    constexpr uint16_t CKSEL_HOCO = 1;
    constexpr uint16_t CKSEL_MAIN = 2;
    constexpr uint16_t CKSEL_SUB = 3;
    constexpr uint16_t CKSEL_PLL = 4;

    // PLLCR (sec.9.2.5 p.355). PLIDIV 00/01/10 = /1,/2,/3 and 11 is prohibited, so the
    // divisor is field+1 over the legal range. STC's printed table runs 010011b (x10.0) to
    // 111011b (x30.0) in half steps with no gaps, i.e. (STC+1)/2; outside that range the
    // setting is prohibited.
    constexpr uint16_t PLLCR_PLIDIV_MASK = 0x3u;
    constexpr uint16_t PLLCR_PLLSRCSEL = 1u << 4; // 0 = main clock oscillator, 1 = HOCO
    constexpr unsigned PLLCR_STC_S = 8;
    constexpr uint16_t PLLCR_STC_MASK = 0x3Fu;
    constexpr uint16_t PLLCR_STC_MIN = 0x13u; // x10.0
    constexpr uint16_t PLLCR_STC_MAX = 0x3Bu; // x30.0

    // HOCOCR2.HCFRQ[1:0] (sec.9.2.13 p.364), readable whatever HOCOCR.HCSTP says; 11b is
    // prohibited.
    constexpr uint8_t HOCOCR2_HCFRQ_MASK = 0x3u;
    constexpr uint32_t HOCO_16MHZ = 16000000u;
    constexpr uint32_t HOCO_18MHZ = 18000000u;
    constexpr uint32_t HOCO_20MHZ = 20000000u;

    // On-chip oscillator nominals (Table 9.1 p.346). LOCO is specified 240 kHz +/-10%
    // (216..264 kHz, sec.9.2.17 p.370), so a rate derived off it is nominal, not exact.
    constexpr uint32_t LOCO_HZ = 240000u;
    constexpr uint32_t SUB_OSC_HZ = 32768u;

    // THE ONE LEAF NO REGISTER CARRIES: nothing reports the crystal's frequency.
    // MOFCR.MODRV2 encodes a DRIVE-CAPABILITY band, and sec.9.2.19 p.372 warns that "a
    // setting value may not fit within the frequency range depending on a crystal". Board
    // fact: 24 MHz crystal (board UM r12uz0098ej0110 Table 1-1).
    constexpr uint32_t MAIN_OSC_HZ = 24000000u;
}

#endif
