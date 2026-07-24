// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RX72M SCI6 (board console UART) register offsets + fields (Phase-1 register
// consolidation). From the RX72M Group User's Manual: Hardware
// (r01uh0804ej0120, Rev.1.20) sec.42; hand-rolled, clean-room. ADDITIVE:
// duplicates the literals still inline in chip_rx72m.cc. Bases: mmap.h.

#ifndef KICKOS_ARCH_RX_CHIP_RX72M_REGS_SCI_H
#define KICKOS_ARCH_RX_CHIP_RX72M_REGS_SCI_H

#include <stdint.h>

#include "../mmap.h"

namespace kickos::rx::reg::sci
{
    // SCI6 register addresses (UM sec.42), byte-wide, from the SCI6 base.
    constexpr uintptr_t SMR = mmap::SCI6 + 0x00;  // serial mode
    constexpr uintptr_t BRR = mmap::SCI6 + 0x01;  // bit rate
    constexpr uintptr_t SCR = mmap::SCI6 + 0x02;  // serial control
    constexpr uintptr_t TDR = mmap::SCI6 + 0x03;  // transmit data
    constexpr uintptr_t SSR = mmap::SCI6 + 0x04;  // serial status
    constexpr uintptr_t SEMR = mmap::SCI6 + 0x07; // serial extended mode

    // SCR fields (UM sec.42.2.6).
    constexpr uint8_t SCR_TE = 1u << 5;   // transmit enable
    constexpr uint8_t SCR_TIE = 1u << 7;  // transmit-data-empty interrupt enable
    // SSR fields (UM sec.42.2.7).
    constexpr uint8_t SSR_TDRE = 1u << 7; // transmit-data-empty
    // SEMR fields (UM sec.42.2.13).
    constexpr uint8_t SEMR_ABCS = 1u << 4; // 8 base-clock cycles per bit
    constexpr uint8_t SEMR_BGDM = 1u << 6; // baud generator double-speed

    // Async 8N1 at 115200 from PCLKB=60 MHz. With BGDM=1, ABCS=1, SMR.CKS=00:
    //   N = PCLKB/(16 * 2^(2n-1) * B) - 1 = 60e6/(8*115200) - 1 = 64.1 -> 64
    // Actual baud 60e6/(8*65) = 115385 (+0.16%). (UM sec.42 async baud table.)
    constexpr uint8_t BRR_115200 = 64;
    constexpr uint8_t SEMR_115200 = SEMR_BGDM | SEMR_ABCS; // 0x50
}

#endif
