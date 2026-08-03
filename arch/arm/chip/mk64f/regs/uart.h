// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// MK64FN1M0 UART register map (K64 Sub-Family RM ch.52). BYTE-mapped register
// file: every access is an 8-bit load/store (a wider load at S1 spans S2/C3/D and
// reading D pops the RX FIFO). Offsets are instance-relative to a UARTn base.

#ifndef KICKOS_ARCH_ARM_CHIP_MK64F_REGS_UART_H
#define KICKOS_ARCH_ARM_CHIP_MK64F_REGS_UART_H

#include <stdint.h>

namespace kickos::mk64f::reg::uart
{
    constexpr uintptr_t BDH_OFFSET = 0x00u;
    constexpr uintptr_t BDL_OFFSET = 0x01u;
    constexpr uintptr_t C1_OFFSET = 0x02u;
    constexpr uintptr_t C2_OFFSET = 0x03u;
    constexpr uintptr_t S1_OFFSET = 0x04u;
    constexpr uintptr_t S2_OFFSET = 0x05u;
    constexpr uintptr_t C3_OFFSET = 0x06u;
    constexpr uintptr_t D_OFFSET = 0x07u;
    constexpr uintptr_t C4_OFFSET = 0x0Au;
    constexpr uintptr_t C5_OFFSET = 0x0Bu;
    constexpr uintptr_t MODEM_OFFSET = 0x0Du;
    constexpr uintptr_t IR_OFFSET = 0x0Eu;
    constexpr uintptr_t PFIFO_OFFSET = 0x10u;
    constexpr uintptr_t CFIFO_OFFSET = 0x11u;
    constexpr uintptr_t C7816_OFFSET = 0x18u;

    // C2 (RM 52.3.4) "can be read or written at any time": TIE/RIE may be toggled on a
    // live channel with no TE/RE clear.
    constexpr uint8_t C2_TIE = 1u << 7; // transmit-interrupt enable: IRQ while S1.TDRE
    constexpr uint8_t C2_TCIE = 1u << 6;
    constexpr uint8_t C2_RIE = 1u << 5; // receive-interrupt enable: IRQ while S1.RDRF
    constexpr uint8_t C2_ILIE = 1u << 4;
    constexpr uint8_t C2_TE = 1u << 3;
    constexpr uint8_t C2_RE = 1u << 2;

    // S1 (RM 52.3.5), reset 0xC0 (TDRE and TC already set with nothing transmitted).
    // Read-only, and every flag clears by reading S1 with the flag set and then
    // reading or writing D, never by writing S1.
    //
    // TDRE and RDRF are WATERMARK comparisons, not fixed conditions: TDRE means "TX
    // datawords <= TWFIFO.TXWATER" and RDRF means "RX datawords >= RWFIFO.RXWATER"
    // (RM 52.3.19 / 52.3.21). They mean buffer-empty and one-byte-available only while
    // the FIFOs stay disabled (PFIFO reset, depth 1) with the reset watermarks
    // TXWATER=0 / RXWATER=1. Enabling PFIFO without setting the watermarks changes what
    // both flags mean.
    constexpr uint8_t S1_TDRE = 1u << 7; // transmit-data-register empty
    constexpr uint8_t S1_TC = 1u << 6;   // transmit complete: shifter idle (not just TDRE)
    constexpr uint8_t S1_RDRF = 1u << 5; // receive-data-register full
    constexpr uint8_t S1_IDLE = 1u << 4;
    constexpr uint8_t S1_OR = 1u << 3; // receiver overrun; BLOCKS RDRF and IDLE while set
    constexpr uint8_t S1_NF = 1u << 2;
    constexpr uint8_t S1_FE = 1u << 1; // framing error; INHIBITS reception until cleared
    constexpr uint8_t S1_PF = 1u << 0;

    // The four receive-error flags. They are reported on the UART0 ERROR vector (IRQ 32),
    // NOT the status vector (IRQ 31), yet they appear in the same S1 a status handler
    // reads. OR blocks RDRF while set, so a handler that ignores them wedges RX.
    constexpr uint8_t S1_RX_ERRORS = S1_OR | S1_NF | S1_FE | S1_PF;

    // C1 frame format (RM 52.3.3). M selects a 9-bit frame; with M=0 and PE=1 the parity
    // bit REPLACES the 8th data bit (RM 52.4.4.1 Table 52-11), so 8 data bits plus parity
    // needs M=1.
    constexpr uint8_t C1_M = 1u << 4;
    constexpr uint8_t C1_PE = 1u << 1;
    constexpr uint8_t C1_PT = 1u << 0; // 0 even, 1 odd

    // BDH (RM 52.3.1): SBR[12:8] plus SBNS. A BDH write is buffered and takes effect only
    // when BDL is written, so BDH must be written first.
    constexpr uint8_t BDH_SBR_MASK = 0x1Fu;
    constexpr uint8_t BDH_SBNS = 1u << 5; // 0 = one stop bit, 1 = two

    constexpr uint8_t C4_BRFA_MASK = 0x1Fu; // baud fine-adjust, in 1/32 increments

    // CFIFO command bits (RM 52.3.17): write 1 to flush the TX / RX FIFO buffer.
    constexpr uint8_t CFIFO_RXFLUSH = 1u << 6;
    constexpr uint8_t CFIFO_TXFLUSH = 1u << 7;

    // MODEM.TXCTSE (RM 52.3.14): CTS hardware flow control. If set, the polled
    // writer waits forever on an absent CTS and drops every byte (SILENT LOSS).
    constexpr uint8_t MODEM_TXCTSE = 1u << 0;
}

#endif // KICKOS_ARCH_ARM_CHIP_MK64F_REGS_UART_H
