// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Infineon XMC4800 USIC shared-IP layer. The USIC is one hardware block that
// implements UART(ASC)/SPI(SSC)/I2C(IIC)/I2S; this header models a USIC channel
// by its base address and exposes the USIC-common mechanism (module/kernel
// clock, baud generator, input-stage mux, FIFO partition, raw TX/RX/status)
// independent of the selected protocol. The ASC (UART) protocol layer lives in
// usic_uart.cc; a future SSC/IIC driver reuses the same ops.
//
// Register addresses and bit fields are clean-room from the XMC4700/XMC4800
// Reference Manual (V1.3, 2016-07); no XMCLib/DAVE/CMSIS vendor source. "RM
// p.NN" citations are the manual's printed page numbers.

#ifndef KICKOS_ARCH_ARM_CHIP_XMC4800_USIC_H
#define KICKOS_ARCH_ARM_CHIP_XMC4800_USIC_H

#include <stddef.h>
#include <stdint.h>

namespace kickos
{
namespace xmc
{
namespace usic
{
    inline volatile uint32_t& reg32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }

    // Channel base addresses (RM Table 18-21, "Registers Address Space"). A 2nd
    // channel or a 2nd USIC module is just another base plus its own SCU
    // gate/reset bit (see module_clock_enable): USIC0_CH1 = 0x40030200,
    // USIC1_CH0 = 0x48020000, USIC2_CH0 = 0x48024000.
    constexpr uintptr_t U0C0_BASE = 0x40030000;

    // Per-channel register offsets from the channel base (RM Table 18-20, "USIC
    // Kernel-Related and Kernel Registers").
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
        constexpr uintptr_t CMTR = 0x044;   // Capture Mode Timer (capture-only; inert for ASC TX)
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

    // SCU clock-gating / peripheral-reset (RM p.11-*). USIC0 is bit 11 in both
    // SCU_CGATCLR0 and SCU_PRCLR0 (RM 18.10 initialization sequence). USIC1/USIC2
    // live in the CGATCLR1/PRCLR1 pair with their own bit; module_clock_enable
    // takes both register addresses and the bit so a second module is reachable.
    constexpr uintptr_t SCU_CGATCLR0 = 0x50004648; // CCU 0x600 + CGATCLR0 0x048
    constexpr uintptr_t SCU_PRCLR0 = 0x50004414;   // RCU 0x400 + PRCLR0 0x014
    constexpr uint32_t SCU_USIC0_BIT = 1u << 11;

    // KSCFG (RM p.18-165): MODEN(0) enables the module kernel clock; BPMODEN(1)
    // is the write-enable for MODEN (reads as 0). Both must be 1 to switch on.
    constexpr uint32_t KSCFG_MODEN = 1u << 0;
    constexpr uint32_t KSCFG_BPMODEN = 1u << 1;

    // FDR (RM p.18-178): fractional divider. DM[15:14]=10B selects fractional
    // mode -> fFD = fPERIPH * STEP/1024. STEP[9:0].
    constexpr uint32_t FDR_DM_FRACTIONAL = 0x2u << 14;
    constexpr uint32_t FDR_STEP_MASK = 0x3FFu;

    // BRG (RM p.18-179) field positions. CLKSEL[1:0]=00B (fPIN=fFD), CTQSEL[7:6]
    // =00B (fCTQIN=fPDIV), PPPEN(4)=0 -> left zero by set_baud.
    constexpr uint32_t BRG_PCTQ_SHIFT = 8;  // [9:8]
    constexpr uint32_t BRG_DCTQ_SHIFT = 10; // [14:10]
    constexpr uint32_t BRG_PDIV_SHIFT = 16; // [25:16]

    // DXnCR (RM p.18-173): DSEL[2:0] selects the input line DXnA..DXnG; INSW(4)=0
    // keeps the shift-unit input under the protocol pre-processor. select_input
    // writes DSEL with INSW/other fields 0.
    constexpr uint32_t DX_DSEL_MASK = 0x7u;

    // TCSR.TDV (RM p.18-189): the transmit buffer still holds a word pending
    // transfer -> not ready. TCSR is the same across protocols.
    constexpr uint32_t TCSR_TDV = 1u << 7;

    // FMR.MTDV[1:0] (RM p.18-193): 10B clears TCSR.TDV (and TE); 01B would SET TDV.
    // Reclaim writes 10B to drop a stale TDV word a dead-baud driver may have left:
    // with TDV=0 the pending TBUF word is gated off (TCSR.TDEN starts a transfer
    // only while TDV=1), so it is never sent before the polled panic banner. Write-
    // only; TCSR control writes do not clear TDV.
    constexpr uint32_t FMR_MTDV_CLEAR = 0x2u << 0;

    // CCR.TBIEN (RM p.18-160): Transmit-Buffer Interrupt Enable -- fires the
    // standard-transmit-buffer interrupt when a word moves TBUF->shifter (TBUF now
    // free). The drain trigger for the buffered console; toggled by tx_irq_*.
    constexpr uint32_t CCR_TBIEN = 1u << 13;

    // INPR.TBINP[6:4] (RM p.18-*): Transmit-Buffer interrupt node pointer -- picks
    // which service-request output SR0..SR5 the TB interrupt drives (set at init).
    constexpr uint32_t INPR_TBINP_SHIFT = 4;
    constexpr uint32_t INPR_TBINP_MASK = 0x7u << 4;

    // PSR.BUSY (RM p.18-70): a data transfer is in progress. Present in the
    // streaming modes (ASC/SSC); reserved in IIS. With PCR.TSTEN=1 it reflects
    // true TX end-of-frame, unlike TCSR.TDV which clears one frame early at the
    // buffer->shifter handoff (RM p.18-189).
    constexpr uint32_t PSR_BUSY = 1u << 9;

    // RBUFSR (RM p.18-204): RDV0(13)/RDV1(14) "Receive Data Valid" -> the dual
    // standard receive buffer holds an unread word; reading RBUF releases it and
    // clears the corresponding RDV. DSR[15:0] in RBUF holds the received word.
    constexpr uint32_t RBUFSR_RDV0 = 1u << 13;
    constexpr uint32_t RBUFSR_RDV1 = 1u << 14;
    constexpr uint32_t RBUF_DSR_MASK = 0xFFFFu;

    // FIFO buffer control TBCTR/RBCTR field positions (RM p.18-214 / 18-218). The
    // module has one shared 64-entry FIFO RAM split across both channels' TX and
    // RX regions: DPTR[5:0] is the start index into that RAM, SIZE[26:24] the
    // region size (coding below), LIMIT[13:8] the fill-level interrupt trigger.
    constexpr uint32_t FIFO_DPTR_SHIFT = 0;   // [5:0]
    constexpr uint32_t FIFO_LIMIT_SHIFT = 8;  // [13:8]
    constexpr uint32_t FIFO_SIZE_SHIFT = 24;  // [26:24]
    // SIZE coding (RM p.18-216): 0=disabled, n=2^n entries (1->2 .. 6->64).
    constexpr uint32_t FIFO_SIZE_DISABLED = 0u;

    // Precomputed baud-generator parameters. No runtime solver: a new baud is a
    // documented hand-calc of the RM formula (see set_baud) yielding one of these.
    // Fields are the raw register values written by set_baud.
    struct Baud
    {
        uint16_t step; // FDR.STEP[9:0]
        uint16_t pdiv; // BRG.PDIV[9:0 within field]; divider = pdiv+1
        uint8_t pctq;  // BRG.PCTQ[1:0]; count  = pctq+1
        uint8_t dctq;  // BRG.DCTQ[4:0]; count  = dctq+1 (tq per bit)
    };

    // 115200 baud from fPERIPH = 60 MHz (see set_baud for the derivation):
    // STEP=755, PDIV+1=24, PCTQ+1=1, DCTQ+1=16 -> 115203.9 baud (+0.0033%).
    constexpr Baud BAUD_115200_60MHZ = { 755u, 23u, 0u, 15u };

    // 115200 baud from fPERIPH = 72 MHz (fCPU=144 MHz profile; same derivation):
    // STEP=367, PDIV+1=14, PCTQ+1=1, DCTQ+1=16 -> 115199.5 baud (-0.0004%).
    constexpr Baud BAUD_115200_72MHZ = { 367u, 13u, 0u, 15u };

    // ---- Generic USIC-common operations (all take the channel base) ----------

    // Ungate then de-reset the module via SCU. Ordering is mandatory (RM 11.6:
    // clock must be active before the individual peripheral reset is released).
    void module_clock_enable(uintptr_t cgatclr, uintptr_t prclr, uint32_t bit);

    // Enable the module kernel clock (KSCFG MODEN|BPMODEN) with the RM-recommended
    // read-back before any further USIC register access (RM p.18-165).
    void kernel_clock_enable(uintptr_t base);

    // Program the baud-rate generator: FDR fractional divider then BRG dividers.
    void set_baud(uintptr_t base, Baud const& b);

    // Select an input line for input stage DXn (DXnCR.DSEL); dxn_off is off::DXnCR.
    void select_input(uintptr_t base, uintptr_t dxn_off, uint32_t dsel);

    // Partition the shared FIFO RAM for a channel region. ctr_off is off::TBCTR
    // (TX) or off::RBCTR (RX); size uses the SIZE coding above (0 disables).
    // Polled console UART does not need this; it exists for a future SSC/IIC user.
    void configure_fifo(uintptr_t base, uintptr_t ctr_off, uint32_t dptr, uint32_t size, uint32_t limit);

    // Raw TX status/data (single-shot, no polling loop -- the caller bounds waits).
    bool tx_ready(uintptr_t base); // TCSR.TDV clear: transmit buffer can take a word
    void tx_put(uintptr_t base, uint8_t v);
    bool tx_idle(uintptr_t base);  // PSR.BUSY clear: shifter has emptied

    // Transmit-buffer interrupt gate (buffered console drain trigger). tx_irq_route
    // selects the service-request output SRx (0..5) once at init; enable/disable
    // toggle CCR.TBIEN while the channel runs.
    void tx_irq_route(uintptr_t base, uint32_t srx);
    void tx_irq_enable(uintptr_t base);
    void tx_irq_disable(uintptr_t base);

    // Raw RX status/data.
    bool rx_ready(uintptr_t base); // a received word is waiting in the buffer
    uint8_t rx_get(uintptr_t base); // read + release the receive buffer

    // Read PSR, clear the masked flags via PSCR, return the masked bits that were
    // set. Used by the protocol layer to sample+clear error flags.
    uint32_t status_read_clear(uintptr_t base, uint32_t mask);
}
}
}

#endif
