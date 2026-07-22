// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Infineon XMC4800 USIC ASC-mode (UART) polled console -- the thin ASC protocol
// layer over the shared USIC-IP mechanism in usic.h (module/kernel clock, baud
// generator, input mux, raw TX/RX/status). ASC-specific register setup
// (SCTR/TCSR/PCR/CCR + the pin mux) lives here. Register addresses and bit
// fields are clean-room from the XMC4700/XMC4800 Reference Manual (V1.3,
// 2016-07); no XMCLib/DAVE/CMSIS vendor source. "RM p.NN" citations are the
// manual's printed page numbers.
//
// Target: the XMC4800 Relax Kit on-board J-Link-OB virtual COM port. Per the
// board User Manual (Table 5): the debugger's UART TX line (PC_RX) is driven by
// XMC pin P1.5 = U0C0.DOUT, and the debugger's RX line (PC_TX) feeds XMC pin
// P1.4 = U0C0.DX0B. So the channel is USIC0 channel 0 (U0C0), TX on P1.5, RX on
// P1.4. This matches the RM's ASC BootStrap-Loader mapping: "Port pins used are
// P1.4 (U0C0_DX0B) for USIC RX and P1.5 (U0C0_DOUT0) for USIC" (RM p.19-*).
//
// Clock: fPERIPH = fCPU/2 = 72 MHz after the crystal PLL bring-up in
// chip_xmc4800.cc clock_init() (runs before this). Baud is 115200.

#include "usic.h"

#include <kickos/console_tx.h>

#include <stddef.h>
#include <stdint.h>

extern "C"
{
    // fCPU (drives fPERIPH = fCPU/2). Defined in chip_xmc4800.cc; the baud re-derive on
    // a clock-select reads the LANDED value here.
    extern uint32_t SystemCoreClock;
}

namespace
{
    namespace u = kickos::xmc::usic;

    constexpr uintptr_t U0C0 = u::U0C0_BASE;

    // --- PORT1 (RM p.26-*): P1 base 0x48028100; IOCR4 controls P1.[7:4] -------
    // Pn_IOCR0 = 0x48028010 + n*0x100, IOCR4 = IOCR0 + 4.
    constexpr uintptr_t P1_IOCR4 = 0x48028114;
    // PCx field is 5 bits: PC4 at [7:3], PC5 at [15:11] (RM Table for Pn_IOCR4).
    constexpr uint32_t PC4_SHIFT = 3;
    constexpr uint32_t PC5_SHIFT = 11;
    constexpr uint32_t PC_MASK = 0x1Fu;
    // PCx coding (RM Table 26-5): 10010B = push-pull, alternate output func 2.
    // P1.5's ALT2 is U0C0.DOUT0 (RM Table 26-12 "Port I/O Functions"), the TX.
    constexpr uint32_t PC_PP_ALT2 = 0x12u;
    // 00000B = input, direct, no internal pull device. P1.4 is the RX input; the
    // USIC reads it through the DX0 input stage regardless of output config.
    constexpr uint32_t PC_INPUT = 0x00u;

    // DX0CR.DSEL = 001B selects input line DX0B = P1.4 (RX) (RM p.18-173).
    constexpr uint32_t DX0_DSEL_B = 0x1u;

    // SCTR (RM p.18-183): SDIR(0)=0 LSB first; PDL(1)=1 idle/passive line high
    // (UART idle); TRM[9:8]=01B shift control active-high (required for any
    // transfer); FLE[21:16]=7 frame length 8 bits; WLE[27:24]=7 word length
    // 8 bits (WLE=N means N+1 bits); DSM[3:2]=00B one bit through DX0/DOUT0.
    constexpr uint32_t SCTR_PDL = 1u << 1;
    constexpr uint32_t SCTR_TRM_ACTIVE = 0x1u << 8;
    constexpr uint32_t SCTR_FLE_8 = 7u << 16;
    constexpr uint32_t SCTR_WLE_8 = 7u << 24;

    // TCSR (RM p.18-186): TDEN[11:10]=01B start a transfer when TDV=1;
    // TDSSM(8)=1 single-shot so a buffered word is not resent (TDV clears at the
    // transmit-buffer event).
    constexpr uint32_t TCSR_TDEN_TDV = 0x1u << 10;
    constexpr uint32_t TCSR_TDSSM = 1u << 8;

    // PCR ASC mode (RM p.18-67): SMD(0)=1 majority sample (3 samples/bit);
    // STPB(1)=0 one stop bit; SP[12:8]=9 sample point (must be <= DCTQ);
    // TSTEN(17)=1 exposes the transfer-in-progress status in PSR.BUSY, used to
    // drain the shifter without clearing anything (BUSY is type r; RM p.18-70).
    constexpr uint32_t PCR_ASC_SMD = 1u << 0;
    constexpr uint32_t PCR_ASC_SP = 9u << 8;
    constexpr uint32_t PCR_ASC_TSTEN = 1u << 17;

    // CCR (RM p.18-160): MODE[3:0]=2 selects the ASC (UART) protocol; writing
    // this last enables the channel once everything else is configured.
    constexpr uint32_t CCR_MODE_ASC = 0x2u;

    // ASC-mode PSR line-error flags (RM p.18-70/71): RNS(4) receiver noise,
    // FER0(5)/FER1(6) stop-bit framing errors, DLIF(11) data-lost (RX overrun).
    // Parity is not enabled (CCR.PM=00B) so no parity flag is sampled here.
    constexpr uint32_t PSR_RNS = 1u << 4;
    constexpr uint32_t PSR_FER0 = 1u << 5;
    constexpr uint32_t PSR_FER1 = 1u << 6;
    constexpr uint32_t PSR_DLIF = 1u << 11;
    constexpr uint32_t ASC_ERR_MASK = PSR_RNS | PSR_FER0 | PSR_FER1 | PSR_DLIF;

    // Bounded so a misconfigured baud/enable NEVER hangs arch_console_write (it
    // feeds the kernel banner and every kos_print). Cap far exceeds any real
    // per-byte wait at 115200 baud. Modeled on the rp2040 backend's wait_mask.
    constexpr uint32_t TX_POLL_TIMEOUT = 1000000u;

    bool tx_wait_ready()
    {
        for (uint32_t i = 0; i < TX_POLL_TIMEOUT; i++)
        {
            if (u::tx_ready(U0C0))
            {
                return true;
            }
        }
        return false;
    }

    // Wait until the shifter has clocked out the full frame (PSR.BUSY clear).
    // Call only after tx_wait_ready() has confirmed the buffer->shifter handoff,
    // so BUSY is guaranteed already set and this cannot return one frame early.
    bool tx_wait_idle()
    {
        for (uint32_t i = 0; i < TX_POLL_TIMEOUT; i++)
        {
            if (u::tx_idle(U0C0))
            {
                return true;
            }
        }
        return false;
    }

    // --- Buffered console TX backend (console_tx.h). The ring drains via the
    // USIC0 transmit-buffer interrupt (CCR.TBIEN) routed to SR0. ---
    int xmc_tx_slot_free(void)
    {
        if (u::tx_ready(U0C0))
        {
            return 1;
        }
        return 0;
    }
    void xmc_tx_push(uint8_t b) { u::tx_put(U0C0, b); }
    void xmc_tx_irq_enable(void) { u::tx_irq_enable(U0C0); }
    void xmc_tx_irq_disable(void) { u::tx_irq_disable(U0C0); }

    constexpr uint32_t CONSOLE_TX_SIZE = 512; // power of two; > kprintf's 256B buffer
    char console_tx_buf[CONSOLE_TX_SIZE];
    console_tx_backend const xmc_console_backend = {
        xmc_tx_slot_free, xmc_tx_push, xmc_tx_irq_enable, xmc_tx_irq_disable};

    // USIC0 service-request 0 -> NVIC line 84 (USIC0_0_IRQn). Confirm against the
    // XMC4800 RM interrupt-node-assignment table; a wrong line silently never
    // drains (the ring fills and falls back to the bounded sync path).
    constexpr int USIC0_SR0_IRQ = 84;
}

extern "C"
{

void kickos_xmc_usic_init(void)
{
    // Module clock on, out of reset (ungate before de-reset; RM 11.6), then the
    // kernel clock. USIC0 = bit 11 in the CGATCLR0/PRCLR0 pair.
    u::module_clock_enable(u::SCU_CGATCLR0, u::SCU_PRCLR0, u::SCU_USIC0_BIT);
    u::kernel_clock_enable(U0C0);

    // Baud rate generator (fractional divider + ASC bit-time dividers).
    u::set_baud(U0C0, u::BAUD_115200_72MHZ);

    // Shift + transmit + protocol config while the channel is still disabled.
    u::reg32(U0C0 + u::off::SCTR) = SCTR_WLE_8 | SCTR_FLE_8 | SCTR_TRM_ACTIVE | SCTR_PDL;
    u::reg32(U0C0 + u::off::TCSR) = TCSR_TDEN_TDV | TCSR_TDSSM;
    u::reg32(U0C0 + u::off::PCR) = PCR_ASC_SP | PCR_ASC_SMD | PCR_ASC_TSTEN;
    u::reg32(U0C0 + u::off::PSCR) = 0xFFFFFFFFu; // clear any stale protocol status flags

    // Route the RX input (P1.4 -> DX0B) into the ASC pre-processor. Input-stage
    // config must be done while CCR.MODE=0 (RM p.18-57).
    u::select_input(U0C0, u::off::DX0CR, DX0_DSEL_B);

    // Route the transmit-buffer interrupt to service-request output SR0 (NVIC
    // line 84); TBIEN stays clear -- the console ring primes it on the first write.
    u::tx_irq_route(U0C0, 0);

    // Enable the channel by selecting the ASC protocol (config must be complete
    // before this write).
    u::reg32(U0C0 + u::off::CCR) = CCR_MODE_ASC;

    // Pins LAST: P1.5 -> push-pull ALT2 (U0C0.DOUT0, TX); P1.4 -> input (RX). The
    // RM (p.18-57/58) requires the output ALT function be enabled only AFTER the
    // ASC mode is active, or the idle DOUT level can spike a spurious start bit.
    // RMW so P1.6/P1.7 (PC6/PC7) in the same IOCR4 are left untouched.
    uint32_t iocr = u::reg32(P1_IOCR4);
    iocr &= ~((PC_MASK << PC4_SHIFT) | (PC_MASK << PC5_SHIFT));
    iocr |= (PC_INPUT << PC4_SHIFT) | (PC_PP_ALT2 << PC5_SHIFT);
    u::reg32(P1_IOCR4) = iocr;
}

// Panic-path reclaim (console.cc D6): force U0C0 back to a known polled-ready ASC
// channel after a userspace driver may have garbled EVERY writable register inside
// its granted 0x200 window. Runs with IRQs masked, privileged; MUST be idempotent +
// re-entrant, so it is straight-line ABSOLUTE stores only -- NO read-modify-write on
// any driver-touched register (an RMW on a garbled value is not safe to repeat from a
// nested-fault re-entry). Overrides the weak no-op in console.cc.
//
// Reclaim depth = rewrite every in-window writable register init sets (baud/mode/DMA/
// IRQ) PLUS the ones init leaves at reset default that a hostile driver can set to
// cause SILENT LOSS -- here KSCFG.MODEN (module clock gate). Registers OUTSIDE the
// window (SCU_CGATCLR0/PRCLR0 system clock gate, P1_IOCR4 pin mux) are privileged and
// unreachable by the driver -> intact -> not touched.
void arch_console_reclaim(void)
{
    // (a) Module kernel clock FIRST. The driver can clear KSCFG.MODEN (window offset
    // 0x00C), which gates the channel kernel clock; with it off EVERY later write here
    // is silently dropped and the banner is lost. kernel_clock_enable writes
    // MODEN|BPMODEN (absolute) and does the RM-mandated read-back before further access.
    u::kernel_clock_enable(U0C0);

    // (b) Stop the channel and any driver FIFO/DMA/mode before reprogramming. Disabling
    // via CCR.MODE=0 halts an in-flight transfer; the FIFO controls may have been armed
    // by the driver (init never touches them).
    u::reg32(U0C0 + u::off::CCR) = 0;
    u::reg32(U0C0 + u::off::TBCTR) = 0;
    u::reg32(U0C0 + u::off::RBCTR) = 0;

    // (c) Re-establish baud + full ASC config to the exact init values. TCSR absolute
    // store also clears any DMA-trigger / interrupt-enable bits the driver set.
    u::set_baud(U0C0, u::BAUD_115200_72MHZ); // FDR + BRG
    u::reg32(U0C0 + u::off::SCTR) = SCTR_WLE_8 | SCTR_FLE_8 | SCTR_TRM_ACTIVE | SCTR_PDL;
    u::reg32(U0C0 + u::off::TCSR) = TCSR_TDEN_TDV | TCSR_TDSSM;
    u::reg32(U0C0 + u::off::PCR) = PCR_ASC_SP | PCR_ASC_SMD | PCR_ASC_TSTEN;
    u::select_input(U0C0, u::off::DX0CR, DX0_DSEL_B); // DX0 RX input mux

    // (d) Drop a stale Transmit-Data-Valid word a hostile driver may have loaded into
    // TBUF (TDV=1): FMR.MTDV=10B clears TDV so the pending word is gated off and never
    // sent (TCSR.TDEN starts a transfer only while TDV=1). Absolute write; TCSR control
    // writes above do not clear the TDV status bit. (This does NOT remove the leading
    // reconfig byte -- see the KNOWN ARTIFACT note at (e).)
    u::reg32(U0C0 + u::off::FMR) = u::FMR_MTDV_CLEAR;

    // (e) Clear stale protocol status flags, then re-enable the channel LAST (config
    // must be complete before the enabling MODE write). TBIEN stays clear: panic is
    // polled, not IRQ-driven.
    //
    // KNOWN ARTIFACT: a driver that clears SCTR.PDL (passive level -> 0) drives the ASC
    // TX pin (P1.5 ALT2 = DOUT) LOW; the line stays low across the fault and this reclaim
    // and only returns to idle-high at the SCTR (PDL=1) write above / this MODE re-enable.
    // The receiver frames that single low->high recovery edge as ONE spurious leading
    // byte (~0xC0) before the banner. It is a physical line-recovery transient, not a
    // TBUF/TDV word (clearing TDV at (d) does not remove it); the banner + dump that
    // follow are byte-clean. Unavoidable from the TX side once the line has been pinned
    // low past a frame boundary.
    u::reg32(U0C0 + u::off::PSCR) = 0xFFFFFFFFu;
    u::reg32(U0C0 + u::off::CCR) = CCR_MODE_ASC;
}

void kickos_xmc_usic_write(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        if (not tx_wait_ready())
        {
            return; // give up rather than hang the caller on a misconfiguration
        }
        u::tx_put(U0C0, static_cast<uint8_t>(buf[i]));
    }
    // Drain before returning: the caller frequently sleeps (WFI) right after a
    // print. clock_init() sets SLEEPCR.SYSSEL=1 so WFI no longer drops the USIC
    // clock to fOFI mid-shift (the root cause of the old garbled last byte), so
    // this is now a pure status drain: wait for the buffer->shifter handoff (TDV
    // clear) then for the shifter to empty (PSR.BUSY clear), both bounded.
    if (tx_wait_ready())
    {
        (void)tx_wait_idle();
    }
}

// Clock-select console coherence (arch.h). fPERIPH = fCPU/2 tracks a clock-select, so
// the USIC baud MUST be re-derived after a retune, and no byte may be mid-shift at the
// old baud when fPERIPH moves. Both run under the caller's IrqLock (see cpu_clock_set).
//
// flush_sync: the generic step already poll-drained the software ring into TBUF; wait
// for the buffer->shifter handoff (TDV clear) then the shifter to empty (PSR.BUSY clear),
// both bounded -- the exact drain kickos_xmc_usic_write does after a print.
void arch_console_flush_sync(void)
{
    if (tx_wait_ready())
    {
        (void)tx_wait_idle();
    }
}

// retune: reprogram the baud generator (FDR + BRG) for the new fPERIPH = SystemCoreClock/2,
// selecting the precomputed point for the landed clock. SILICON-PENDING: the live baud
// reprogram (channel enabled, but idle + IRQs masked here) is validated on the Relax Kit
// in the separate silicon pass. An unrecognized clock leaves the baud untouched (a P-state
// whose fPERIPH has no in-tolerance divisor should be rejected at the seam -- ruling 2).
void arch_console_retune(void)
{
    u::Baud b;
    switch (SystemCoreClock)
    {
    case 144000000u: b = u::BAUD_115200_72MHZ; break; // fPERIPH 72 MHz
    case 96000000u:  b = u::BAUD_115200_48MHZ; break; // fPERIPH 48 MHz
    case 48000000u:  b = u::BAUD_115200_24MHZ; break; // fPERIPH 24 MHz
    default: return;                                   // unknown clock: do not touch baud
    }
    u::set_baud(U0C0, b);
}

// Non-blocking RX drain: copy up to n received words into buf, return the count
// read. The DX0 input (P1.4) is already routed to the ASC pre-processor by
// kickos_xmc_usic_init(). This is the reusable polled-RX foundation -- there is
// no kernel console-input consumer yet. No FIFO: the single-word standard
// receive buffer means a caller that does not keep up loses bytes.
size_t kickos_xmc_usic_read(char* buf, size_t n)
{
    size_t got = 0;
    while (got < n and u::rx_ready(U0C0))
    {
        buf[got] = static_cast<char>(u::rx_get(U0C0));
        got++;
    }
    return got;
}

// Sample and clear the ASC line-error flags (framing/noise/data-lost). Returns
// the raw PSR error bits that were set (0 = clean line). HW-unvalidated.
uint32_t kickos_xmc_usic_errors(void)
{
    return u::status_read_clear(U0C0, ASC_ERR_MASK);
}

console_tx_backend const* arch_console_tx_backend(char** storage, uint32_t* size, int* irq_line)
{
    *storage = console_tx_buf;
    *size = CONSOLE_TX_SIZE;
    *irq_line = USIC0_SR0_IRQ;
    return &xmc_console_backend;
}

}
