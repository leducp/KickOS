// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Infineon XMC4800 USIC ASC-mode (UART) console on USIC0 channel 0 (U0C0), on the
// DOUT0 and DX0 pins the board file names. Register facts are from the XMC4700/XMC4800
// Reference Manual (V1.3, 2016-07); "RM p.NN" citations are its printed page numbers.

#include "usic.h"

#include "board_pins.h"
#include "irq.h"
#include "regs/port.h"
#include "regs/scu.h"
#include "regs/usic.h"

#include <kickos/arch/arch.h>
#include <kickos/console_tx.h>

#include <stddef.h>
#include <stdint.h>

namespace
{
    namespace u = kickos::xmc::usic;
    namespace ru = kickos::xmc::reg::usic;
    namespace rp = kickos::xmc::reg::port;
    namespace rs = kickos::xmc::reg::scu;

    constexpr uintptr_t U0C0 = u::U0C0_BASE;

    // The window arch_console_reclaim rewrites, and the one the driver is granted. ONE
    // constant, because a reclaim that rewrote a register outside the window it reports
    // would rewrite registers whose holder was never checked.
    constexpr uintptr_t CONSOLE_WIN_BASE = U0C0;
    constexpr size_t CONSOLE_WIN_SIZE = KICKOS_BOARD_CONSOLE_SIZE;

    // Every offset the reclaim body writes must lie inside that window. Adding a store
    // outside it fails to build instead of silently widening the reclaim's reach.
    static_assert(u::off::KSCFG < CONSOLE_WIN_SIZE and u::off::FDR < CONSOLE_WIN_SIZE
                      and u::off::BRG < CONSOLE_WIN_SIZE and u::off::DX0CR < CONSOLE_WIN_SIZE
                      and u::off::SCTR < CONSOLE_WIN_SIZE and u::off::TCSR < CONSOLE_WIN_SIZE
                      and u::off::PCR < CONSOLE_WIN_SIZE and u::off::CCR < CONSOLE_WIN_SIZE
                      and u::off::PSCR < CONSOLE_WIN_SIZE and u::off::FMR < CONSOLE_WIN_SIZE
                      and u::off::TBCTR < CONSOLE_WIN_SIZE and u::off::RBCTR < CONSOLE_WIN_SIZE,
                  "arch_console_reclaim writes outside the window it reports");

    static_assert(KICKOS_BOARD_CONSOLE_BASE == U0C0, "the board's console is not the channel this backend drives");

    constexpr uintptr_t TX_IOCR = KICKOS_BOARD_CONSOLE_DOUT0_PORT_BASE + rp::iocr_off(KICKOS_BOARD_CONSOLE_DOUT0_BIT);
    constexpr uintptr_t RX_IOCR = KICKOS_BOARD_CONSOLE_DX0_PORT_BASE + rp::iocr_off(KICKOS_BOARD_CONSOLE_DX0_BIT);
    constexpr uint32_t TX_SHIFT = rp::pc_shift(KICKOS_BOARD_CONSOLE_DOUT0_BIT);
    constexpr uint32_t RX_SHIFT = rp::pc_shift(KICKOS_BOARD_CONSOLE_DX0_BIT);
    constexpr uint32_t TX_PC = rp::pc_pp_alt(KICKOS_BOARD_CONSOLE_DOUT0_SELECT);

    // A read-modify-write, so the other pins of each IOCR keep their fields.
    void iocr_set(uintptr_t iocr, uint32_t clear, uint32_t set)
    {
        uint32_t v = u::reg32(iocr);
        v &= ~clear;
        v |= set;
        u::reg32(iocr) = v;
    }

    // Bounded so a misconfigured baud/enable NEVER hangs arch_console_write (it
    // feeds the kernel banner and every kos_print). The cap far exceeds any real
    // per-byte wait at 115200 baud.
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
    char console_tx_buf[KICKOS_CONSOLE_TX_SIZE];
    console_tx_backend const xmc_console_backend = {
        xmc_tx_slot_free, xmc_tx_push, xmc_tx_irq_enable, xmc_tx_irq_disable};
}

extern "C"
{

void kickos_xmc_usic_init(void)
{
    // Module clock on and out of reset before the kernel clock.
    u::module_clock_enable(rs::CGATCLR0, rs::PRCLR0, rs::USIC0_GATE_BIT);
    u::kernel_clock_enable(U0C0);

    // Baud rate generator (fractional divider + ASC bit-time dividers), for the branch
    // clock this boot actually landed on. A bring-up that fell back to fOFI leaves
    // fPERIPH at 24 MHz, which the table does cover; an fPERIPH it does not cover leaves
    // the generator alone, which is the same refusal arch_console_retune makes.
    u::Baud b;
    if (u::baud_for_periph(arch_periph_clock_hz(U0C0), &b))
    {
        u::set_baud(U0C0, b);
    }

    // Shift + transmit + protocol config while the channel is still disabled.
    u::reg32(U0C0 + u::off::SCTR) = ru::SCTR_WLE_8 | ru::SCTR_FLE_8 | ru::SCTR_TRM_ACTIVE | ru::SCTR_PDL;
    u::reg32(U0C0 + u::off::TCSR) = ru::TCSR_TDEN_TDV | ru::TCSR_TDSSM;
    u::reg32(U0C0 + u::off::PCR) = ru::PCR_ASC_SP | ru::PCR_ASC_SMD | ru::PCR_ASC_TSTEN;
    u::reg32(U0C0 + u::off::PSCR) = 0xFFFFFFFFu;

    // Route the RX pin's DX0 input into the ASC pre-processor. Input-stage config must be
    // done while CCR.MODE=0 (RM p.18-57).
    u::select_input(U0C0, u::off::DX0CR, KICKOS_BOARD_CONSOLE_DX0_INPUT_SELECT);

    // Route the transmit-buffer interrupt to service-request output SR0 (NVIC
    // line 84); TBIEN stays clear, the console ring primes it on the first write.
    u::tx_irq_route(U0C0, 0);

    // Enable the channel by selecting the ASC protocol (config must be complete
    // before this write).
    u::reg32(U0C0 + u::off::CCR) = ru::CCR_MODE_ASC;

    // Pins LAST: the RM (p.18-57/58) requires the output ALT function be enabled only AFTER
    // the ASC mode is active, or the idle DOUT level can spike a spurious start bit.
    if constexpr (TX_IOCR == RX_IOCR)
    {
        iocr_set(TX_IOCR, (rp::PC_FIELD_MASK << RX_SHIFT) | (rp::PC_FIELD_MASK << TX_SHIFT),
                 (rp::PC_INPUT_NOPULL << RX_SHIFT) | (TX_PC << TX_SHIFT));
    }
    else
    {
        iocr_set(RX_IOCR, rp::PC_FIELD_MASK << RX_SHIFT, rp::PC_INPUT_NOPULL << RX_SHIFT);
        iocr_set(TX_IOCR, rp::PC_FIELD_MASK << TX_SHIFT, TX_PC << TX_SHIFT);
    }
}

// USIC0 CH0, one channel of the module (RM Table 18-21). On xmcuartirq the IRQ thread
// holds this window, and that is NOT the thread whose death notes the console dead.
void arch_console_reclaim_window(uintptr_t* base, size_t* size)
{
    *base = CONSOLE_WIN_BASE;
    *size = CONSOLE_WIN_SIZE;
}

// Panic-path reclaim: force U0C0 back to a known polled-ready ASC
// channel after a userspace driver may have garbled EVERY writable register inside
// its granted 0x200 window. Runs with IRQs masked, privileged; MUST be idempotent +
// re-entrant, so every store here is ABSOLUTE, NO read-modify-write on any
// driver-touched register: an RMW on a garbled value is not safe to repeat from a
// nested-fault re-entry. The one exception is the baud rewrite below, gated on
// baud_for_periph resolving a divisor for the live peripheral clock; skipping it on
// failure is itself idempotent.
//
// Reclaim depth = every in-window writable register init sets (baud/mode/DMA/IRQ) plus
// the ones init leaves at reset default that a hostile driver can set to cause SILENT
// LOSS, here KSCFG.MODEN (module clock gate). Registers OUTSIDE the window
// (SCU_CGATCLR0/PRCLR0 system clock gate, the pins' IOCR mux) are privileged and out of
// the driver's reach.
void arch_console_reclaim(void)
{
    // (a) Module kernel clock FIRST. The driver can clear KSCFG.MODEN (window offset
    // 0x00C), which gates the channel kernel clock; with it off EVERY later write here
    // is silently dropped and the banner is lost. kernel_clock_enable writes
    // MODEN|BPMODEN (absolute) and does the RM-mandated read-back before further access.
    u::kernel_clock_enable(CONSOLE_WIN_BASE);

    // (b) Stop the channel and any driver FIFO/DMA/mode before reprogramming. Disabling
    // via CCR.MODE=0 halts an in-flight transfer; the FIFO controls may have been armed
    // by the driver (init never touches them).
    u::reg32(CONSOLE_WIN_BASE + u::off::CCR) = 0;
    u::reg32(CONSOLE_WIN_BASE + u::off::TBCTR) = 0;
    u::reg32(CONSOLE_WIN_BASE + u::off::RBCTR) = 0;

    // (c) Re-establish baud + full ASC config to the exact init values. TCSR absolute
    // store also clears any DMA-trigger / interrupt-enable bits the driver set. The baud
    // comes off the live branch clock, as at init: a reclaim that reprogrammed the
    // 72 MHz point would garble a console that had been working at another rate.
    u::Baud b;
    if (u::baud_for_periph(arch_periph_clock_hz(CONSOLE_WIN_BASE), &b))
    {
        u::set_baud(CONSOLE_WIN_BASE, b);
    }
    u::reg32(CONSOLE_WIN_BASE + u::off::SCTR) = ru::SCTR_WLE_8 | ru::SCTR_FLE_8 | ru::SCTR_TRM_ACTIVE | ru::SCTR_PDL;
    u::reg32(CONSOLE_WIN_BASE + u::off::TCSR) = ru::TCSR_TDEN_TDV | ru::TCSR_TDSSM;
    u::reg32(CONSOLE_WIN_BASE + u::off::PCR) = ru::PCR_ASC_SP | ru::PCR_ASC_SMD | ru::PCR_ASC_TSTEN;
    u::select_input(CONSOLE_WIN_BASE, u::off::DX0CR, KICKOS_BOARD_CONSOLE_DX0_INPUT_SELECT);
    u::reg32(CONSOLE_WIN_BASE + u::off::INPR) = 0; // TBINP back to SR0, which a driver may have moved

    // (d) Drop a stale Transmit-Data-Valid word a hostile driver may have loaded into
    // TBUF (TDV=1): FMR.MTDV=10B clears TDV so the pending word is gated off and never
    // sent (TCSR.TDEN starts a transfer only while TDV=1). Absolute write; TCSR control
    // writes above do not clear the TDV status bit. This does NOT remove the leading
    // reconfig byte; see the KNOWN ARTIFACT note at (e).
    u::reg32(CONSOLE_WIN_BASE + u::off::FMR) = u::FMR_MTDV_CLEAR;

    // (e) Clear stale protocol status flags, then re-enable the channel LAST (config
    // must be complete before the enabling MODE write). TBIEN stays clear: panic is
    // polled, not IRQ-driven.
    //
    // KNOWN ARTIFACT: a driver that clears SCTR.PDL (passive level -> 0) drives the ASC
    // TX pin (DOUT0) LOW; the line stays low across the fault and this reclaim
    // and only returns to idle-high at the SCTR (PDL=1) write above / this MODE re-enable.
    // The receiver frames that single low->high recovery edge as ONE spurious leading
    // byte (~0xC0) before the banner. It is a physical line-recovery transient, not a
    // TBUF/TDV word (clearing TDV at (d) does not remove it); the banner + dump that
    // follow are byte-clean. Unavoidable from the TX side once the line has been pinned
    // low past a frame boundary.
    u::reg32(CONSOLE_WIN_BASE + u::off::PSCR) = 0xFFFFFFFFu;
    u::reg32(CONSOLE_WIN_BASE + u::off::CCR) = ru::CCR_MODE_ASC;
}

bool kickos_xmc_usic_write(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        if (not tx_wait_ready())
        {
            return false; // give up rather than hang the caller on a misconfiguration
        }
        u::tx_put(U0C0, static_cast<uint8_t>(buf[i]));
    }
    // Drained before returning: the caller frequently sleeps (WFI) right after a print.
    if (not tx_wait_ready())
    {
        return false;
    }
    return tx_wait_idle();
}

// Clock-select console coherence (arch.h). fPERIPH = fCPU/2 tracks a clock-select, so
// the USIC baud MUST be re-derived after a retune, and no byte may be mid-shift at the
// old baud when fPERIPH moves. Both run under the caller's IrqLock (see cpu_clock_set).
//
// flush_sync: the generic step already poll-drained the software ring into TBUF; wait
// for the buffer->shifter handoff (TDV clear) then the shifter to empty (PSR.BUSY
// clear), both bounded, as kickos_xmc_usic_write does after a print.
void arch_console_flush_sync(void)
{
    if (tx_wait_ready())
    {
        (void)tx_wait_idle();
    }
}

// retune: reprogram the baud generator (FDR + BRG) for the new fPERIPH, selecting the
// precomputed point for the landed clock. The reprogram is live (the channel stays
// enabled) but is reached only with the channel idle and IRQs masked. An unrecognized
// clock leaves the baud untouched: a P-state whose fPERIPH has no in-tolerance divisor is
// rejected at the seam.
void arch_console_retune(void)
{
    u::Baud b;
    if (u::baud_for_periph(arch_periph_clock_hz(U0C0), &b))
    {
        u::set_baud(U0C0, b);
    }
}

// Non-blocking RX drain: copy up to n received words into buf, return the count
// read. No FIFO: the standard receive buffer holds two words (RDV0/RDV1), so a caller
// that does not keep up loses bytes.
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
    return u::status_read_clear(U0C0, ru::ASC_ERR_MASK);
}

console_tx_backend const* arch_console_tx_backend(char** storage, uint32_t* size, int* irq_line)
{
    *storage = console_tx_buf;
    *size = KICKOS_CONSOLE_TX_SIZE;
    *irq_line = kickos::xmc::irq::USIC0_SR0;
    return &xmc_console_backend;
}

}
