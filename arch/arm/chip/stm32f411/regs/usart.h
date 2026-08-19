// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// STM32F411 USART register map (RM0383 sec.19.6), classic SR/DR layout. Clean-room from
// RM0383 Rev 4; no vendor HAL/CMSIS source.
//
// TWO SPELLINGS OF THE SAME REGISTER. The *_OFFSET constants are instance-relative and are
// what a driver granted one window uses; the bare names below are the console instance
// (USART2) resolved absolutely, for the kernel console which is wired to that one channel.
//
// Every access must be a half-word or word (RM0383 sec.19.6): the byte lanes are not
// individually addressable.

#ifndef KICKOS_ARCH_ARM_CHIP_STM32F411_REGS_USART_H
#define KICKOS_ARCH_ARM_CHIP_STM32F411_REGS_USART_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::stm32f411::reg::usart
{
    // Instance-relative offsets (RM0383 sec.19.6.8 Table 88). GTPR is the last register at
    // 0x18, so the file spans 0x00 to 0x1B and BLOCK_SIZE covers all of it with 4 bytes of
    // slack: the smallest PMSAv7 region that reaches every register, well under the 1 KB
    // memory-map slot RM0383 sec.2.3 reserves for USART2.
    constexpr uintptr_t SR_OFFSET = 0x00u;
    constexpr uintptr_t DR_OFFSET = 0x04u;
    constexpr uintptr_t BRR_OFFSET = 0x08u;
    constexpr uintptr_t CR1_OFFSET = 0x0Cu;
    constexpr uintptr_t CR2_OFFSET = 0x10u;
    constexpr uintptr_t CR3_OFFSET = 0x14u;
    constexpr uintptr_t GTPR_OFFSET = 0x18u;
    constexpr uintptr_t BLOCK_SIZE = 0x20u;

    // The console instance, absolute.
    constexpr uintptr_t SR = mmap::USART2_BASE + SR_OFFSET;
    constexpr uintptr_t DR = mmap::USART2_BASE + DR_OFFSET;
    constexpr uintptr_t BRR = mmap::USART2_BASE + BRR_OFFSET;
    constexpr uintptr_t CR1 = mmap::USART2_BASE + CR1_OFFSET;
    constexpr uintptr_t CR2 = mmap::USART2_BASE + CR2_OFFSET;
    constexpr uintptr_t CR3 = mmap::USART2_BASE + CR3_OFFSET;
    constexpr uintptr_t GTPR = mmap::USART2_BASE + GTPR_OFFSET;

    // SR (RM0383 sec.19.6.1).
    constexpr uint32_t SR_PE = 1u << 0;   // parity error
    constexpr uint32_t SR_FE = 1u << 1;   // framing error
    constexpr uint32_t SR_NF = 1u << 2;   // noise detected on a frame that still framed
    constexpr uint32_t SR_ORE = 1u << 3;  // overrun
    constexpr uint32_t SR_IDLE = 1u << 4; // idle line detected
    constexpr uint32_t SR_RXNE = 1u << 5; // read data register not empty
    constexpr uint32_t SR_TC = 1u << 6;   // transmission complete
    constexpr uint32_t SR_TXE = 1u << 7;  // TX data register empty
    constexpr uint32_t SR_LBD = 1u << 8;  // LIN break detected
    constexpr uint32_t SR_CTS = 1u << 9;  // CTS transition

    // Every one of these is cleared by a read of SR followed by an access to DR
    // (RM0383 sec.19.6.1). ORE in particular is NOT cleared by a bare DR read, and with
    // RXNEIE armed it holds the single USART vector asserted forever.
    constexpr uint32_t SR_RX_ERRORS = SR_PE | SR_FE | SR_NF | SR_ORE;

    // CR1 (RM0383 sec.19.6.4).
    constexpr uint32_t CR1_SBK = 1u << 0;
    constexpr uint32_t CR1_RWU = 1u << 1;
    constexpr uint32_t CR1_RE = 1u << 2;      // receiver enable
    constexpr uint32_t CR1_TE = 1u << 3;      // transmitter enable
    constexpr uint32_t CR1_IDLEIE = 1u << 4;
    // Arms ORE as well as RXNE (RM0383 sec.19.4 Table 86); FE and NF reach the vector only
    // through CR3.EIE with CR3.DMAR set, which is why an 8N1 driver sees neither.
    constexpr uint32_t CR1_RXNEIE = 1u << 5;
    constexpr uint32_t CR1_TCIE = 1u << 6;
    constexpr uint32_t CR1_TXEIE = 1u << 7;   // TXE interrupt enable
    constexpr uint32_t CR1_PEIE = 1u << 8;
    constexpr uint32_t CR1_PS = 1u << 9;      // 0 = even, 1 = odd
    constexpr uint32_t CR1_PCE = 1u << 10;    // parity control enable
    constexpr uint32_t CR1_WAKE = 1u << 11;
    // M = 0 is an 8-bit frame TOTAL: parity REPLACES the eighth data bit
    // (RM0383 sec.19.3.7 Table 85). 8 data bits plus parity is M = 1.
    constexpr uint32_t CR1_M = 1u << 12;
    constexpr uint32_t CR1_UE = 1u << 13;    // USART enable
    constexpr uint32_t CR1_OVER8 = 1u << 15; // 0 = 16x oversampling, which BRR below assumes

    // CR2 (RM0383 sec.19.6.5). STOP is bits 13:12 and 2 stop bits is 0b10, NOT 0b01: 01 is
    // the half stop bit of Smartcard mode, which frames every byte wrong at the right baud.
    constexpr uint32_t CR2_STOP_SHIFT = 12;
    constexpr uint32_t CR2_STOP_MASK = 0x3u << CR2_STOP_SHIFT;
    constexpr uint32_t CR2_STOP_1 = 0u << CR2_STOP_SHIFT;
    constexpr uint32_t CR2_STOP_2 = 2u << CR2_STOP_SHIFT;
    constexpr uint32_t CR2_LBCL = 1u << 8;
    constexpr uint32_t CR2_CPHA = 1u << 9;
    constexpr uint32_t CR2_CPOL = 1u << 10;
    constexpr uint32_t CR2_CLKEN = 1u << 11;
    constexpr uint32_t CR2_LINEN = 1u << 14;

    // CR3 (RM0383 sec.19.6.6). Each of the four named in the reclaim body breaks the wire
    // silently rather than loudly: CTSE postpones every byte until an unwired CTS is
    // asserted, IREN modulates the TX pin, HDSEL ties TX and RX into one wire, SCEN adds
    // smartcard framing.
    constexpr uint32_t CR3_EIE = 1u << 0;
    constexpr uint32_t CR3_IREN = 1u << 1;
    constexpr uint32_t CR3_IRLP = 1u << 2;
    constexpr uint32_t CR3_HDSEL = 1u << 3;
    constexpr uint32_t CR3_NACK = 1u << 4;
    constexpr uint32_t CR3_SCEN = 1u << 5;
    constexpr uint32_t CR3_DMAR = 1u << 6;
    constexpr uint32_t CR3_DMAT = 1u << 7;
    constexpr uint32_t CR3_RTSE = 1u << 8;
    constexpr uint32_t CR3_CTSE = 1u << 9;
    constexpr uint32_t CR3_CTSIE = 1u << 10;
    constexpr uint32_t CR3_ONEBIT = 1u << 11;

    // OVER8=0: BRR = fPCLK1 / baud, with BRR[15:4]=mantissa, BRR[3:0]=fraction/16
    // (RM0383 sec.19.3.4 and sec.19.6.3). Rounded to nearest at compute time.
    constexpr uint32_t BRR_MASK = 0xFFFFu;
    // USARTDIV below 1 turns the generator off, so BRR under 16 is not a rate.
    constexpr uint32_t BRR_MIN = 16u;
    constexpr uint32_t BAUD_115200 = 115200u;
}

#endif
