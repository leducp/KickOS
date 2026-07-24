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

    constexpr uint8_t C2_TIE = 1u << 7; // transmit-interrupt enable: IRQ while S1.TDRE
    constexpr uint8_t C2_TE = 1u << 3;
    constexpr uint8_t C2_RE = 1u << 2;

    constexpr uint8_t S1_TDRE = 1u << 7; // transmit-data-register empty
    constexpr uint8_t S1_TC = 1u << 6;   // transmit complete: shifter idle (not just TDRE)

    // CFIFO command bits (RM 60.3.9): write 1 to flush the TX / RX FIFO buffer.
    constexpr uint8_t CFIFO_RXFLUSH = 1u << 6;
    constexpr uint8_t CFIFO_TXFLUSH = 1u << 7;

    // MODEM.TXCTSE (RM 60.3.13): CTS hardware flow control -- if set, the polled
    // writer waits forever on an absent CTS and drops every byte (SILENT LOSS).
    constexpr uint8_t MODEM_TXCTSE = 1u << 0;
}

#endif // KICKOS_ARCH_ARM_CHIP_MK64F_REGS_UART_H
