// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RX72M SCI6 (board console UART) register offsets + fields. From the RX72M Group
// User's Manual: Hardware (r01uh0804ej0120, Rev.1.20) sec.42; hand-rolled,
// clean-room. Bases: mmap.h.

#ifndef KICKOS_ARCH_RX_CHIP_RX72M_REGS_SCI_H
#define KICKOS_ARCH_RX_CHIP_RX72M_REGS_SCI_H

#include <stdint.h>

#include <kickos/chip_mmap.h>

namespace kickos::rx::reg::sci
{
    // SCI6 register addresses (UM sec.42), byte-wide, from the SCI6 base.
    constexpr uintptr_t SMR = mmap::SCI6 + 0x00;  // serial mode
    constexpr uintptr_t BRR = mmap::SCI6 + 0x01;  // bit rate
    constexpr uintptr_t SCR = mmap::SCI6 + 0x02;  // serial control
    constexpr uintptr_t TDR = mmap::SCI6 + 0x03;  // transmit data
    constexpr uintptr_t SSR = mmap::SCI6 + 0x04;  // serial status
    constexpr uintptr_t RDR = mmap::SCI6 + 0x05;  // receive data (UM sec.42.2.2 p.2157)
    constexpr uintptr_t SEMR = mmap::SCI6 + 0x07; // serial extended mode

    // SCR fields, asynchronous mode (UM sec.42.2.10 p.2167).
    constexpr uint8_t SCR_TIE = 1u << 7;  // TXI interrupt enable
    constexpr uint8_t SCR_RIE = 1u << 6;  // RXI *and* ERI interrupt enable
    constexpr uint8_t SCR_TE = 1u << 5;   // transmit enable
    constexpr uint8_t SCR_RE = 1u << 4;   // receive enable
    constexpr uint8_t SCR_TEIE = 1u << 2; // TEI interrupt enable

    // TXI generation, exhaustive (UM sec.42.12.2(1) p.2308): a TDR -> TSR transfer with
    // TIE already 1, TE 0 -> 1 with TIE already 1, TE and TIE set by one instruction, and
    // TE 1 -> 0 with TIE 1. Setting TIE while TE is 1 raises nothing, hence RULE T1: arm
    // TIE and write the first byte in the same pass, or TX never starts.
    // With TIE armed, ANY TDR write re-raises the line a TX pass is servicing, and the
    // edge rearm redelivers it, so such a pass must write TDR only to drain the ring.
    // Clear TIE only with the ring empty: it also discards the request the SCI retains
    // internally (sec.42.12.1(1) p.2308). Masking at the interrupt controller is not a
    // substitute, because a tier-1 rearm unmasks on every wait.

    // SSR fields, asynchronous mode (UM sec.42.2.11 p.2171).
    constexpr uint8_t SSR_TDRE = 1u << 7; // transmit-data-empty
    constexpr uint8_t SSR_RDRF = 1u << 6; // receive-data-full; cleared by READING RDR
    constexpr uint8_t SSR_ORER = 1u << 5; // overrun
    constexpr uint8_t SSR_FER = 1u << 4;  // framing
    constexpr uint8_t SSR_PER = 1u << 3;  // parity
    constexpr uint8_t SSR_TEND = 1u << 2; // transmission end; READ-ONLY
    constexpr uint8_t SSR_ERRORS = SSR_ORER | SSR_FER | SSR_PER;
    // Clearing ORER/FER/PER takes a read-1-then-write-0 ("confirm that the flag is 1 and
    // then set it to 0", Note 1 p.2171), and a write of the SSR byte must put 1 in the
    // TDRE and RDRF positions (Note 2: "Write 1 when writing is necessary"), so a plain
    // read-modify-write that stores the value just read is wrong. RECEPTION IS STOPPED
    // while any of the three is 1 (sec.42.3.9 p.2241: "Data reception cannot be resumed
    // while the receive error flag is 1"), and overrun handling must also READ RDR
    // (Fig.42.21 step [6] p.2243) or the next frame is not received correctly.

    // SEMR fields (UM sec.42.2.15 p.2195).
    constexpr uint8_t SEMR_ABCS = 1u << 4; // 8 base-clock cycles per bit
    constexpr uint8_t SEMR_BGDM = 1u << 6; // baud generator double-speed

    // Async 8N1 at 115200 from PCLKB=60 MHz. With BGDM=1, ABCS=1, SMR.CKS=00:
    //   N = PCLKB/(16 * 2^(2n-1) * B) - 1 = 60e6/(8*115200) - 1 = 64.1 -> 64
    // Actual baud 60e6/(8*65) = 115385 (+0.16%). (UM sec.42 async baud table.)
    constexpr uint8_t BRR_115200 = 64;
    // The channel's actual rate, which a CONFIGURE must report back: echoing the
    // requested 115200 would hide the +0.16%.
    constexpr uint32_t BAUD_115200_ACTUAL = 115385;
    constexpr uint8_t SEMR_115200 = SEMR_BGDM | SEMR_ABCS; // 0x50
}

#endif
