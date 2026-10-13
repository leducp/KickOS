// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Infineon XMC4800 USIC ASC-mode (UART) console on USIC0 channel 0 (U0C0), on the DOUT0 and DX0
// pins the board file names. "RM p.NN" is the XMC4700/XMC4800 RM V1.3 printed page.

#include "usic.h"

#include "board_pins.h"
#include "irq.h"
#include "regs/port.h"
#include "regs/scu.h"
#include "regs/usic.h"

#include <kickos/arch/arch.h>
#include <kickos/arch/console_poll.h>
#include <kickos/config/limits.h>
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

    // The window arch_console_reclaim rewrites and the one the driver is granted: ONE constant, so
    // a reclaim cannot reach registers whose holder was never checked.
    constexpr uintptr_t CONSOLE_WIN_BASE = U0C0;
    constexpr size_t CONSOLE_WIN_SIZE = KICKOS_BOARD_CONSOLE_SIZE;

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
        uint32_t v = r32(iocr);
        v &= ~clear;
        v |= set;
        r32(iocr) = v;
    }

    bool tx_wait_ready()
    {
        for (uint32_t i = 0; i < KICKOS_POLL_SPIN_MAX; i++)
        {
            if (u::tx_ready(U0C0))
            {
                return true;
            }
        }
        return false;
    }

    // Waits for the shifter to clock out the full frame. Only after tx_wait_ready() has confirmed
    // the buffer->shifter handoff, so BUSY is already set and this cannot return a frame early.
    bool tx_wait_idle()
    {
        for (uint32_t i = 0; i < KICKOS_POLL_SPIN_MAX; i++)
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
    console_tx_backend const xmc_console_backend = {
        xmc_tx_slot_free, xmc_tx_push, xmc_tx_irq_enable, xmc_tx_irq_disable,
        &kickos_console_tx_storage};
}

extern "C"
{

void kickos_xmc_usic_init(void)
{
    u::module_clock_enable(rs::CGATCLR0, rs::PRCLR0, rs::USIC0_GATE_BIT);
    u::kernel_clock_enable(U0C0);

    // For the branch clock this boot landed on. An fPERIPH the table does not cover leaves the
    // generator alone, the same refusal arch_console_retune makes.
    u::Baud b;
    if (u::baud_for_periph(arch_periph_clock_hz(U0C0), &b))
    {
        u::set_baud(U0C0, b);
    }

    // Config while the channel is still disabled.
    r32(U0C0 + u::off::SCTR) = ru::SCTR_WLE_8 | ru::SCTR_FLE_8 | ru::SCTR_TRM_ACTIVE | ru::SCTR_PDL;
    r32(U0C0 + u::off::TCSR) = ru::TCSR_TDEN_TDV | ru::TCSR_TDSSM;
    r32(U0C0 + u::off::PCR) = ru::PCR_ASC_SP | ru::PCR_ASC_SMD | ru::PCR_ASC_TSTEN;
    r32(U0C0 + u::off::PSCR) = 0xFFFFFFFFu;

    // Input-stage config needs CCR.MODE=0 (RM p.18-57).
    u::select_input(U0C0, u::off::DX0CR, KICKOS_BOARD_CONSOLE_DX0_INPUT_SELECT);

    // TBIEN stays clear; the console ring primes it on the first write.
    u::tx_irq_route(U0C0, 0);

    // The channel enables here, so the config above must be complete.
    r32(U0C0 + u::off::CCR) = ru::CCR_MODE_ASC;

    // Pins LAST: the output ALT function goes on only AFTER the ASC mode is active, or the idle
    // DOUT level can spike a spurious start bit (RM p.18-57/58).
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

// On xmcuartirq the IRQ thread holds this window, NOT the thread whose death notes the console
// dead.
void arch_console_reclaim_window(uintptr_t* base, size_t* size)
{
    *base = CONSOLE_WIN_BASE;
    *size = CONSOLE_WIN_SIZE;
}

// Panic-path reclaim after a userspace driver may have garbled EVERY writable register in its
// 0x200 window. Runs privileged with IRQs masked; MUST be idempotent and re-entrant: every store
// is ABSOLUTE, never a read-modify-write on a driver-touched register, since an RMW on a garbled
// value is not safe to repeat from a nested-fault re-entry. The exception is the baud rewrite,
// whose skip on failure is itself idempotent.
//
// Beyond what init sets, it covers the registers a hostile driver can set to cause SILENT LOSS,
// here KSCFG.MODEN. The system clock gate and the pins' IOCR mux are outside the window.
void arch_console_reclaim(void)
{
    // (a) Kernel clock FIRST: with KSCFG.MODEN cleared EVERY later write here is silently
    // dropped and the banner is lost.
    u::kernel_clock_enable(CONSOLE_WIN_BASE);

    // (b) Stop the channel and any driver FIFO before reprogramming.
    r32(CONSOLE_WIN_BASE + u::off::CCR) = 0;
    r32(CONSOLE_WIN_BASE + u::off::TBCTR) = 0;
    r32(CONSOLE_WIN_BASE + u::off::RBCTR) = 0;

    // (c) The init values. The baud comes off the live branch clock: reprogramming the 72 MHz
    // point would garble a console working at another rate.
    u::Baud b;
    if (u::baud_for_periph(arch_periph_clock_hz(CONSOLE_WIN_BASE), &b))
    {
        u::set_baud(CONSOLE_WIN_BASE, b);
    }
    r32(CONSOLE_WIN_BASE + u::off::SCTR) = ru::SCTR_WLE_8 | ru::SCTR_FLE_8 | ru::SCTR_TRM_ACTIVE | ru::SCTR_PDL;
    r32(CONSOLE_WIN_BASE + u::off::TCSR) = ru::TCSR_TDEN_TDV | ru::TCSR_TDSSM;
    r32(CONSOLE_WIN_BASE + u::off::PCR) = ru::PCR_ASC_SP | ru::PCR_ASC_SMD | ru::PCR_ASC_TSTEN;
    u::select_input(CONSOLE_WIN_BASE, u::off::DX0CR, KICKOS_BOARD_CONSOLE_DX0_INPUT_SELECT);
    r32(CONSOLE_WIN_BASE + u::off::INPR) = 0; // TBINP back to SR0, which a driver may have moved

    // (d) Drop a stale Transmit-Data-Valid word a driver may have loaded into TBUF; the TCSR
    // writes above do not clear TDV.
    r32(CONSOLE_WIN_BASE + u::off::FMR) = u::FMR_MTDV_CLEAR;

    // (e) Clear stale status flags, then re-enable the channel LAST. TBIEN stays clear: panic is
    // polled.
    //
    // KNOWN ARTIFACT: a driver that cleared SCTR.PDL leaves DOUT0 low; the line returns to idle
    // high at the SCTR write above and this MODE re-enable, and the receiver frames that recovery
    // edge as ONE spurious leading byte (~0xC0) before the banner. It is a line transient, not a
    // TBUF word, and unavoidable from the TX side; the banner and dump that follow are clean.
    r32(CONSOLE_WIN_BASE + u::off::PSCR) = 0xFFFFFFFFu;
    r32(CONSOLE_WIN_BASE + u::off::CCR) = ru::CCR_MODE_ASC;
}

bool kickos_xmc_usic_write(char const* buf, size_t n)
{
    if (not kickos::console_poll::write<xmc_tx_slot_free, xmc_tx_push>(buf, n))
    {
        return false;
    }
    // Drained before returning: the caller often sleeps (WFI) right after a print.
    if (not tx_wait_ready())
    {
        return false;
    }
    return tx_wait_idle();
}

// fPERIPH tracks a clock-select, so the baud MUST be re-derived after a retune, and no byte may
// be mid-shift at the old baud. Both run under the caller's IrqLock.
//
// flush_sync waits, bounded, for the buffer->shifter handoff and then the shifter to empty.
void arch_console_flush_sync(void)
{
    if (tx_wait_ready())
    {
        (void)tx_wait_idle();
    }
}

// retune reprograms FDR and BRG live, with the channel idle and IRQs masked. An unrecognized
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

console_tx_backend const* arch_console_tx_backend(int* irq_line)
{
    *irq_line = kickos::xmc::irq::USIC0_SR0;
    return &xmc_console_backend;
}

}
