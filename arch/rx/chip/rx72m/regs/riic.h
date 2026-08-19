// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RX72M RIICa register offsets + fields. From the RX72M Group User's Manual: Hardware
// (r01uh0804ej0120, Rev.1.20) sec.43; hand-rolled, clean-room. Bases: mmap.h.
//
// Offsets are INSTANCE-RELATIVE; the channel stride is 0x20 and the highest offset used is
// 0x13, so one channel is a 32-byte window against the chip's 16-byte protection granule.
// EVERY REGISTER IS 8-BIT ACCESS ONLY (UM Table 5.1 lists no other size for any of them).

#ifndef KICKOS_ARCH_RX_CHIP_RX72M_REGS_RIIC_H
#define KICKOS_ARCH_RX_CHIP_RX72M_REGS_RIIC_H

#include <stdint.h>

#include <kickos/chip_mmap.h>

namespace kickos::rx::reg::riic
{
    // Register offsets from a channel base (UM sec.43.2).
    constexpr uintptr_t ICCR1 = 0x00;  // control 1: ICE/IICRST/CLO + the line monitors
    constexpr uintptr_t ICCR2 = 0x01;  // control 2: BBSY/MST/TRS + the ST/RS/SP requests
    constexpr uintptr_t ICMR1 = 0x02;  // mode 1: CKS divider + the bit counter
    constexpr uintptr_t ICMR2 = 0x03;  // mode 2: SDA output delay + the timeout selects
    constexpr uintptr_t ICMR3 = 0x04;  // mode 3: WAIT/ACKWP/ACKBT + noise filter stages
    constexpr uintptr_t ICFER = 0x05;  // function enable: which detections are armed
    constexpr uintptr_t ICSER = 0x06;  // slave address enables
    constexpr uintptr_t ICIER = 0x07;  // interrupt enables
    constexpr uintptr_t ICSR1 = 0x08;  // status 1: slave address match
    constexpr uintptr_t ICSR2 = 0x09;  // status 2: the transfer flags
    constexpr uintptr_t SARL0 = 0x0A;  // slave address 0, low
    constexpr uintptr_t SARU0 = 0x0B;  // slave address 0, upper + format select
    constexpr uintptr_t SARL1 = 0x0C;
    constexpr uintptr_t SARU1 = 0x0D;
    constexpr uintptr_t SARL2 = 0x0E;
    constexpr uintptr_t SARU2 = 0x0F;
    constexpr uintptr_t ICBRL = 0x10; // bit rate low-level period (sec.43.2.13)
    constexpr uintptr_t ICBRH = 0x11; // bit rate high-level period (sec.43.2.14)
    constexpr uintptr_t ICDRT = 0x12; // transmit data
    constexpr uintptr_t ICDRR = 0x13; // receive data
    constexpr uintptr_t SPAN = 0x14;  // bytes of this channel's register file

    // ICCR1 (sec.43.2.1). Reset 0x1F.
    //
    // SCLO and SDAO DRIVE THE PINS when SOWP is 0 in the SAME store, so a whole-register write
    // of 0 pulls both bus lines low. Every store here therefore starts from ICCR1_IDLE, which
    // holds SOWP set and so leaves the two output bits protected whatever they carry.
    constexpr uint8_t ICCR1_SDAI = 1u << 0;   // R: SDA line level
    constexpr uint8_t ICCR1_SCLI = 1u << 1;   // R: SCL line level
    constexpr uint8_t ICCR1_SDAO = 1u << 2;   // W: 0 drives SDA low (needs SOWP = 0)
    constexpr uint8_t ICCR1_SCLO = 1u << 3;   // W: 0 drives SCL low (needs SOWP = 0)
    constexpr uint8_t ICCR1_SOWP = 1u << 4;   // 1 protects SCLO/SDAO
    constexpr uint8_t ICCR1_CLO = 1u << 5;    // one extra SCL pulse, self-clearing
    constexpr uint8_t ICCR1_IICRST = 1u << 6; // reset (ICE 0) or internal reset (ICE 1)
    constexpr uint8_t ICCR1_ICE = 1u << 7;
    constexpr uint8_t ICCR1_IDLE = ICCR1_SOWP | ICCR1_SCLO | ICCR1_SDAO;

    // ICCR2 (sec.43.2.2). Reset 0x00. ST/RS/SP are REQUESTS the hardware clears itself, and
    // BBSY/MST/TRS are hardware-driven: software writes none of the three.
    constexpr uint8_t ICCR2_ST = 1u << 1;
    constexpr uint8_t ICCR2_RS = 1u << 2;
    constexpr uint8_t ICCR2_SP = 1u << 3;
    constexpr uint8_t ICCR2_TRS = 1u << 5;
    constexpr uint8_t ICCR2_MST = 1u << 6;
    constexpr uint8_t ICCR2_BBSY = 1u << 7;

    // ICMR1 (sec.43.2.3). Reset 0x08. BCWP reads 1 and must be WRITTEN 1 to leave the bit
    // counter alone: a 0 there commits BC[2:0] from the same store.
    constexpr uint8_t ICMR1_BC_MASK = 0x07;
    constexpr uint8_t ICMR1_BCWP = 1u << 3;
    constexpr uint8_t ICMR1_CKS_MASK = 0x70; // PCLKB >> CKS, CKS 0..7
    constexpr unsigned ICMR1_CKS_SHIFT = 4u;
    constexpr uint8_t ICMR1_MTWP = 1u << 7;

    // ICMR3 (sec.43.2.5). Reset 0x00. ACKBT ONLY TAKES A WRITE WHEN ACKWP WAS ALREADY 1 FROM
    // AN EARLIER STORE (Note 1): one store setting both bits leaves ACKBT clear.
    constexpr uint8_t ICMR3_NF_MASK = 0x03; // stages - 1
    constexpr uint8_t ICMR3_ACKBR = 1u << 2;
    constexpr uint8_t ICMR3_ACKBT = 1u << 3; // 1 = NACK the byte being received
    constexpr uint8_t ICMR3_ACKWP = 1u << 4;
    constexpr uint8_t ICMR3_RDRFS = 1u << 5;
    constexpr uint8_t ICMR3_WAIT = 1u << 6; // hold SCL low from the 9th edge until ICDRR is read
    constexpr uint8_t ICMR3_SMBS = 1u << 7;

    // ICFER (sec.43.2.6). Reset 0x72.
    constexpr uint8_t ICFER_TMOE = 1u << 0;  // timeout counter; 0 at reset and left there
    constexpr uint8_t ICFER_MALE = 1u << 1;  // master arbitration-lost detection
    constexpr uint8_t ICFER_NALE = 1u << 2;  // NACK-transmission arbitration-lost detection
    constexpr uint8_t ICFER_SALE = 1u << 3;  // slave arbitration-lost detection
    constexpr uint8_t ICFER_NACKE = 1u << 4; // suspend the transfer on a received NACK
    constexpr uint8_t ICFER_NFE = 1u << 5;   // digital noise filter
    constexpr uint8_t ICFER_SCLE = 1u << 6;  // SCL synchronous circuit (slave clock stretching)
    constexpr uint8_t ICFER_FMPE = 1u << 7;  // fast mode plus, RIIC0 only

    // ICSR2 (sec.43.2.10). Reset 0x00. Every flag is cleared by writing 0 after reading 1.
    constexpr uint8_t ICSR2_TMOF = 1u << 0;
    constexpr uint8_t ICSR2_AL = 1u << 1;
    constexpr uint8_t ICSR2_START = 1u << 2;
    constexpr uint8_t ICSR2_STOP = 1u << 3;
    constexpr uint8_t ICSR2_NACKF = 1u << 4;
    constexpr uint8_t ICSR2_RDRF = 1u << 5;
    constexpr uint8_t ICSR2_TEND = 1u << 6;
    constexpr uint8_t ICSR2_TDRE = 1u << 7;

    // ICBRL / ICBRH (sec.43.2.13, sec.43.2.14): 5-bit counts, upper three bits read as 1.
    constexpr uint8_t ICBR_MASK = 0x1F;
    constexpr uint8_t ICBR_RESERVED = 0xE0;
}

#endif
