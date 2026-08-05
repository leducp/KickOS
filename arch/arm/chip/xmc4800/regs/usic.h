// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 USIC channel registers + bit fields, including the ASC (UART) and
// SSC (SPI) mode layers. Clean-room from the XMC4700/XMC4800 Reference Manual
// (V1.3, 2016-07); no XMCLib/DAVE/CMSIS vendor source. "RM p.NN" are the
// manual's printed pages. A USIC channel is addressed by base (see mmap.h) +
// the offsets below.

#ifndef KICKOS_ARCH_ARM_CHIP_XMC4800_REGS_USIC_H
#define KICKOS_ARCH_ARM_CHIP_XMC4800_REGS_USIC_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::xmc::reg::usic
{
    // Console channel (Relax Kit VCOM): USIC0 channel 0. The SSC (SPI) drivers
    // use the sibling channel 1.
    constexpr uintptr_t U0C0_BASE = mmap::USIC0_CH0_BASE;
    constexpr uintptr_t U0C1_BASE = mmap::USIC0_CH1_BASE;

    // Per-channel register offsets from the channel base (RM Table 18-20).
    namespace off
    {
        constexpr uintptr_t CCFG = 0x004;   // Channel Configuration
        constexpr uintptr_t KSCFG = 0x00C;  // Kernel State Configuration
        constexpr uintptr_t FDR = 0x010;    // Fractional Divider
        constexpr uintptr_t BRG = 0x014;    // Baud Rate Generator
        constexpr uintptr_t INPR = 0x018;   // Interrupt Node Pointer
        constexpr uintptr_t DX0CR = 0x01C;  // Input Control 0 (0x01C + n*4 -> DXnCR)
        constexpr uintptr_t DX1CR = 0x020;
        constexpr uintptr_t DX2CR = 0x024;
        constexpr uintptr_t DX3CR = 0x028;
        constexpr uintptr_t DX4CR = 0x02C;
        constexpr uintptr_t DX5CR = 0x030;
        constexpr uintptr_t SCTR = 0x034;   // Shift Control
        constexpr uintptr_t TCSR = 0x038;   // Transmit Control/Status
        constexpr uintptr_t PCR = 0x03C;    // Protocol Control (mode-dependent)
        constexpr uintptr_t CCR = 0x040;    // Channel Control (MODE select)
        constexpr uintptr_t CMTR = 0x044;   // Capture Mode Timer (inert for ASC TX)
        constexpr uintptr_t PSR = 0x048;    // Protocol Status (mode-dependent)
        constexpr uintptr_t PSCR = 0x04C;   // Protocol Status Clear
        constexpr uintptr_t RBUFSR = 0x050; // Receiver Buffer Status
        constexpr uintptr_t RBUF = 0x054;   // Receiver Buffer (read releases buffer)
        constexpr uintptr_t RBUF0 = 0x05C;  // Receiver Buffer 0
        constexpr uintptr_t RBUF1 = 0x060;  // Receiver Buffer 1
        constexpr uintptr_t FMR = 0x068;    // Flag Modification (TDV/TBI/... modify)
        constexpr uintptr_t TBUF0 = 0x080;  // Transmit Buffer input location 0
        constexpr uintptr_t TBCTR = 0x108;  // Transmit FIFO Buffer Control
        constexpr uintptr_t RBCTR = 0x10C;  // Receive FIFO Buffer Control
        constexpr uintptr_t TRBSR = 0x114;  // Transmit/Receive FIFO Buffer Status
    }

    // KSCFG (RM p.18-165): MODEN(0) enables the module kernel clock; BPMODEN(1)
    // is the write-enable for MODEN (reads as 0). Both must be 1 to switch on.
    constexpr uint32_t KSCFG_MODEN = 1u << 0;
    constexpr uint32_t KSCFG_BPMODEN = 1u << 1;

    // FDR (RM p.18-178): DM[15:14]=10B fractional mode -> fFD = fPERIPH*STEP/1024.
    constexpr uint32_t FDR_DM_FRACTIONAL = 0x2u << 14;
    constexpr uint32_t FDR_STEP_MASK = 0x3FFu; // STEP[9:0]
    constexpr uint32_t FDR_DM_MASK = 0x3u << 14; // DM[15:14]

    // BRG (RM p.18-179) field positions.
    constexpr uint32_t BRG_PCTQ_SHIFT = 8;  // [9:8]
    constexpr uint32_t BRG_DCTQ_SHIFT = 10; // [14:10]
    constexpr uint32_t BRG_PDIV_SHIFT = 16; // [25:16]
    // BRG bits 2, 5, 15 and [27:26] are the register's only reserved bits
    // (RM p.18-181, type r, "Read as 0; should be written with 0"). Every other bit
    // belongs to a writable field.
    constexpr uint32_t BRG_RESERVED_MASK = (1u << 2) | (1u << 5) | (1u << 15) | (0x3u << 26);

    // DXnCR (RM p.18-173): DSEL[2:0] selects the input line DXnA..DXnG.
    constexpr uint32_t DX_DSEL_MASK = 0x7u;

    // TCSR.TDV (RM p.18-189): transmit buffer holds a word pending -> not ready.
    constexpr uint32_t TCSR_TDV = 1u << 7;

    // FMR.MTDV[1:0] (RM p.18-193): 10B clears TCSR.TDV (write-only; TCSR control
    // writes do not clear TDV).
    constexpr uint32_t FMR_MTDV_CLEAR = 0x2u << 0;

    // CCR.TBIEN (RM p.18-160): Transmit-Buffer Interrupt Enable (drain trigger).
    constexpr uint32_t CCR_TBIEN = 1u << 13;

    // INPR.TBINP[6:4] (RM p.18-*): Transmit-Buffer interrupt node pointer.
    constexpr uint32_t INPR_TBINP_SHIFT = 4;
    constexpr uint32_t INPR_TBINP_MASK = 0x7u << 4;

    // PSR.BUSY (RM p.18-70): transfer in progress. With PCR.TSTEN=1 it reflects
    // true TX end-of-frame, unlike TCSR.TDV which clears one frame early.
    constexpr uint32_t PSR_BUSY = 1u << 9;

    // RBUFSR (RM p.18-204): RDV0/RDV1 "Receive Data Valid"; RBUF.DSR[15:0] word.
    constexpr uint32_t RBUFSR_RDV0 = 1u << 13;
    constexpr uint32_t RBUFSR_RDV1 = 1u << 14;
    constexpr uint32_t RBUF_DSR_MASK = 0xFFFFu;

    // FIFO TBCTR/RBCTR field positions (RM p.18-214 / 18-218).
    constexpr uint32_t FIFO_DPTR_SHIFT = 0;   // [5:0]
    constexpr uint32_t FIFO_LIMIT_SHIFT = 8;  // [13:8]
    constexpr uint32_t FIFO_SIZE_SHIFT = 24;  // [26:24]
    // SIZE coding (RM p.18-216): 0=disabled, n=2^n entries.
    constexpr uint32_t FIFO_SIZE_DISABLED = 0u;

    // ---- ASC (UART) mode field constants -------------------------------------

    // SCTR (RM p.18-183): PDL(1)=1 idle/passive line high; TRM[9:8]=01B active;
    // FLE[21:16]=7 frame length 8; WLE[27:24]=7 word length 8 (N+1 bits).
    constexpr uint32_t SCTR_PDL = 1u << 1;
    constexpr uint32_t SCTR_TRM_ACTIVE = 0x1u << 8;
    constexpr uint32_t SCTR_FLE_8 = 7u << 16;
    constexpr uint32_t SCTR_WLE_8 = 7u << 24;

    // TCSR (RM p.18-186): TDEN[11:10]=01B start when TDV=1; TDSSM(8)=1 single-shot.
    constexpr uint32_t TCSR_TDEN_TDV = 0x1u << 10;
    constexpr uint32_t TCSR_TDSSM = 1u << 8;

    // PCR ASC mode (RM p.18-67): SMD(0)=1 majority sample; SP[12:8]=9 sample
    // point (must be <= DCTQ); TSTEN(17)=1 exposes PSR.BUSY.
    constexpr uint32_t PCR_ASC_SMD = 1u << 0;
    constexpr uint32_t PCR_ASC_SP = 9u << 8;
    constexpr uint32_t PCR_ASC_TSTEN = 1u << 17;

    // CCR (RM p.18-160): MODE[3:0]=2 selects ASC; writing it last enables the channel.
    constexpr uint32_t CCR_MODE_ASC = 0x2u;

    // ASC-mode PSR line-error flags (RM p.18-70/71). Parity is not enabled.
    constexpr uint32_t PSR_RNS = 1u << 4;   // receiver noise
    constexpr uint32_t PSR_FER0 = 1u << 5;  // stop-bit framing error 0
    constexpr uint32_t PSR_FER1 = 1u << 6;  // stop-bit framing error 1
    constexpr uint32_t PSR_DLIF = 1u << 11; // data-lost (RX overrun)
    constexpr uint32_t ASC_ERR_MASK = PSR_RNS | PSR_FER0 | PSR_FER1 | PSR_DLIF;

    // DX0CR.DSEL = 001B selects input line DX0B = P1.4 (console RX) (RM p.18-173).
    constexpr uint32_t DX0_DSEL_B = 0x1u;

    // ---- SSC (SPI) mode field constants --------------------------------------
    // Shared by the XMC SSC users: system/driver/xmc4800/xmcssc and
    // user/apps/xmc4800-relax/{xmcspi,xmccshold}. Only atomic offsets/bits live here; each
    // driver assembles its own PCR/TCSR/PSCR write masks from these (mechanism, not policy).

    // Fixed 72 MHz SSC baud profile (fCPU=144 MHz), reused verbatim by all SSC
    // users: FDR fractional mode (DM=10B, FDR_DM_FRACTIONAL above) STEP=367; BRG
    // PDIV+1=14, PCTQ+1=1, DCTQ+1=16 (RM eq.18.8; RM p.18-178 / p.18-179). The
    // exact rate is immaterial over an on-chip loopback, only that the clock runs.
    constexpr uint32_t FDR_STEP_367 = 367u;
    constexpr uint32_t BRG_PDIV_13 = 13u << 16; // PDIV+1 = 14
    constexpr uint32_t BRG_DCTQ_15 = 15u << 10; // DCTQ+1 = 16
    constexpr uint32_t BRG_PCTQ_0 = 0u << 8;    // PCTQ+1 = 1

    // SCTR (RM p.18-183): SDIR(0)=1 MSB-first (SPI bit order); FLE[21:16]=63 (0x3F)
    // leaves the frame length to the software TCSR.SOF/EOF markers (RM 18.4.3.6).
    // SCTR_TRM_ACTIVE / SCTR_WLE_8 / SCTR_FLE_8 are shared with the ASC layer above.
    constexpr uint32_t SCTR_SDIR_MSB = 1u << 0;
    constexpr uint32_t SCTR_FLE_63 = 0x3Fu << 16;

    // TCSR (RM p.18-186..189): SOF(5)/EOF(6) mark the TBUF word as FIRST/LAST of a
    // software-governed frame (sampled when TDV goes valid). TCSR_TDEN_TDV /
    // TCSR_TDSSM are shared with the ASC layer above.
    constexpr uint32_t TCSR_SOF = 1u << 5;
    constexpr uint32_t TCSR_EOF = 1u << 6;

    // PCR [SSC Mode] (RM p.18-169 / p.18-98..100): MSLSEN(0)=1 SSC master (internal
    // MSLS generation); SELCTR(1)=1 direct-select for SELO[7:0]; SELINV(2)=1
    // active-low SELOx; FEM(3)=1 Frame End Mode (MSLS held across a software gap ->
    // one CS bracket per multi-word frame, RM 18.4.5.1); SELO0(bit16)=1 arms SELO0.
    constexpr uint32_t PCR_MSLSEN = 1u << 0;
    constexpr uint32_t PCR_SELCTR_DIRECT = 1u << 1;
    constexpr uint32_t PCR_SELINV_LOW = 1u << 2;
    constexpr uint32_t PCR_FEM = 1u << 3;
    constexpr uint32_t PCR_SELO0 = 1u << 16;

    // DX0CR (RM p.18-173): INSW(4)=1 routes the synchronized input to the data
    // shift unit (SSC receive path); DSEL[2:0]=110B selects internal input "G" =
    // the channel's own transmitter (LOOP-BACK, RM 18.2.3.5), with no port pin.
    constexpr uint32_t DX0CR_INSW = 1u << 4;
    constexpr uint32_t DX0CR_DSEL_G = 0x6u;

    // INPR (RM p.18-168): RINP[10:8]/AINP[14:12] select the service-request output
    // SRx for the receive / alternative-receive interrupts (SR1 here).
    constexpr uint32_t INPR_RINP_SR1 = 1u << 8;
    constexpr uint32_t INPR_AINP_SR1 = 1u << 12;

    // CCR (RM p.18-160): MODE[3:0]=0001B selects the SSC protocol (written last to
    // enable the channel); RIEN(14)/AIEN(15) are the receive interrupt enables.
    constexpr uint32_t CCR_MODE_SSC = 0x1u;
    constexpr uint32_t CCR_RIEN = 1u << 14;
    constexpr uint32_t CCR_AIEN = 1u << 15;
    constexpr uint32_t CCR_MODE_MASK = 0xFu; // MODE[3:0]

    // PSR [SSC Mode] (RM p.18-170 / p.18-102): MSLS(0) current MSLS level; MSLSEV(2)
    // MSLS-changed event (sticky); RIF(14)/AIF(15) receive / alternative-receive
    // complete (first word of a frame -> AIF, later words -> RIF).
    constexpr uint32_t PSR_MSLS = 1u << 0;
    constexpr uint32_t PSR_MSLSEV = 1u << 2;
    constexpr uint32_t PSR_RIF = 1u << 14;
    constexpr uint32_t PSR_AIF = 1u << 15;

    // PSCR (RM p.18-171): W1C. CST0(0) clears MSLS (stops a running frame), CST2(2)
    // clears MSLSEV, CRIF(14)/CAIF(15) clear RIF/AIF.
    constexpr uint32_t PSCR_MSLS = 1u << 0;
    constexpr uint32_t PSCR_MSLSEV = 1u << 2;
    constexpr uint32_t PSCR_CRIF = 1u << 14;
    constexpr uint32_t PSCR_CAIF = 1u << 15;

    // ---- Precomputed baud-generator parameters (raw register field values) ----
    // No runtime solver: a new baud is a documented hand-calc of the RM formula
    // fASC = fPERIPH*STEP/1024 / ((PDIV+1)*(PCTQ+1)*(DCTQ+1)) (RM eq.18.6).
    struct Baud
    {
        uint16_t step; // FDR.STEP[9:0]
        uint16_t pdiv; // BRG.PDIV; divider = pdiv+1
        uint8_t pctq;  // BRG.PCTQ; count = pctq+1
        uint8_t dctq;  // BRG.DCTQ; tq per bit = dctq+1
    };

    // 115200 baud at the labelled fPERIPH. The 48/24 MHz points are formula-derived,
    // not measured on a scope.
    constexpr Baud BAUD_115200_60MHZ = { 755u, 23u, 0u, 15u }; // +0.0033%
    constexpr Baud BAUD_115200_72MHZ = { 367u, 13u, 0u, 15u }; // -0.0004%
    constexpr Baud BAUD_115200_48MHZ = { 354u, 8u, 0u, 15u };  // +0.03%
    constexpr Baud BAUD_115200_24MHZ = { 393u, 4u, 0u, 15u };  // -0.05%
}

#endif
