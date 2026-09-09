// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Bring-up and console transport shared by the RP2040 and the RP2350. Register addresses and
// fields are clean-room from the two datasheets (RP-008371-DS and RP-008373-DS-2); a section
// number written "a / b" cites the RP2040 first and the RP2350 second.
//
// Every name this unit reads is spelled the same way by both chips' regs/ headers, so the
// configured chip decides the addresses and this unit decides the sequences. Where the two
// parts genuinely differ (clock tree, pin mux, reset bit map, bootrom, the RP2350 AMP window
// and its PMSAv8 MPU backend) the code stays in that chip's own backend.

#include <kickos/arch/arch.h>
#include <kickos/config/limits.h>
#include <kickos/console_tx.h>

#include <stdint.h>

#include <kickos/chip_mmap.h>
#include "regs/clocks.h"
#include "regs/pll.h"
#include "regs/resets.h"
#include "regs/timer.h"
#include "regs/uart.h"
#include "rp2xxx.h"

namespace mmap = kickos::rp2xxx::mmap;
namespace reg = kickos::rp2xxx::reg;

using kickos::rp2xxx::POLL_TIMEOUT;
using kickos::rp2xxx::r32;

namespace kickos::rp2xxx
{
    bool wait_mask(uintptr_t addr, uint32_t mask)
    {
        for (uint32_t i = 0; i < POLL_TIMEOUT; i++)
        {
            if ((r32(addr) & mask) == mask)
            {
                return true;
            }
        }
        return false;
    }

    void unreset(uint32_t mask)
    {
        r32(reg::resets::RESET + mmap::ATOMIC_CLR) = mask;
        wait_mask(reg::resets::RESET_DONE, mask); // bounded; best-effort
    }

    // Bring PLL_SYS up to the chip's clk_sys target. Returns false (PLL left powered down) if
    // the VCO never locks, so the caller can stay on the crystal instead of switching clk_sys
    // onto a dead PLL. Datasheet 2.18.2 / 8.6.4 sequence.
    bool pll_sys_lock()
    {
        // Reset the block first so a warm reboot can't run this off stale dividers.
        r32(reg::resets::RESET + mmap::ATOMIC_SET) = reg::resets::PLL_SYS;
        r32(reg::resets::RESET + mmap::ATOMIC_CLR) = reg::resets::PLL_SYS;
        wait_mask(reg::resets::RESET_DONE, reg::resets::PLL_SYS);

        // Load REFDIV + FBDIV BEFORE powering the VCO.
        r32(reg::pll::CS) = reg::pll::CS_REFDIV_1;
        r32(reg::pll::FBDIV_INT) = reg::pll::FBDIV_125;
        // Power up main regulator + VCO (clear PD, VCOPD). DSMPD stays set (integer FBDIV, no
        // delta-sigma); POSTDIVPD stays set until after lock.
        r32(reg::pll::PWR + mmap::ATOMIC_CLR) = reg::pll::PWR_PD | reg::pll::PWR_VCOPD;
        if (not wait_mask(reg::pll::CS, reg::pll::CS_LOCK))
        {
            return false;
        }
        r32(reg::pll::PRIM) = reg::pll::PRIM_POSTDIV;
        r32(reg::pll::PWR + mmap::ATOMIC_CLR) = reg::pll::PWR_POSTDIVPD; // enable post-dividers
        return true;
    }
}

namespace
{
    // The window arch_console_reclaim rewrites: the console UART's whole APB slot, which is
    // the register block plus the XOR/SET/CLR aliases at +0x1000/+0x2000/+0x3000. The aliases
    // must be inside it: a holder granted only an alias writes the very same registers.
    constexpr uintptr_t CONSOLE_WIN_BASE = reg::uart::BASE;
    constexpr size_t CONSOLE_WIN_SIZE = mmap::APB_ATOMIC_WINDOW;

    static_assert(reg::uart::IBRD >= CONSOLE_WIN_BASE
                      and reg::uart::IBRD < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                      and reg::uart::FBRD < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                      and reg::uart::LCR_H < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                      and reg::uart::CR < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                      and reg::uart::IFLS < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                      and reg::uart::IMSC < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                      and reg::uart::DMACR < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE,
                  "arch_console_reclaim writes outside the window it reports");

    // Which of the two divisor pairs clk_peri needs. clocks_init leaves clk_peri on either
    // PLL_SYS or a slower fallback source, and CLOCKS is the hardware's own record of which.
    // The aux mux alone does not answer it: the RP2350 bootrom parks clk_sys on the ROSC
    // through that same mux, so the aux SOURCE has to be PLL_SYS too.
    bool console_clk_on_pll()
    {
        bool const on_aux =
            (r32(reg::clocks::CLK_SYS_SELECTED) & reg::clocks::CLK_SYS_SELECTED_AUX) != 0;
        bool const aux_is_pll =
            (r32(reg::clocks::CLK_SYS_CTRL) & reg::clocks::CLK_SYS_AUXSRC_MASK) ==
            reg::clocks::CLK_SYS_AUXSRC_PLL;
        return on_aux and aux_is_pll;
    }
}

extern "C"
{

void arch_console_write(char const* buf, size_t n)
{
    console_tx_write(buf, n); // buffered; the routing guard (console.cc) keeps this thread-only
}

void arch_console_write_sync(char const* buf, size_t n)
{
    if (n == 0)
    {
        return;
    }
#if KICKOS_AMP_OWN_IMAGE
    kickos::rp2xxx::console_claim();
#endif
    for (size_t i = 0; i < n; i++)
    {
        uint32_t spin = 0;
        while ((r32(reg::uart::FR) & reg::uart::FR_TXFF) != 0)
        {
            if (++spin > KICKOS_POLL_SPIN_MAX)
            {
#if KICKOS_AMP_OWN_IMAGE
                kickos::rp2xxx::console_drop(true); // a wedged channel keeps no claim
#endif
                return; // bounded: a wedged UART must not hang the panic path (drop)
            }
        }
        r32(reg::uart::DR) = static_cast<uint8_t>(buf[i]);
    }
#if KICKOS_AMP_OWN_IMAGE
    kickos::rp2xxx::console_drop(buf[n - 1] == '\n');
#endif
}

void arch_console_reclaim_window(uintptr_t* base, size_t* size)
{
    *base = CONSOLE_WIN_BASE;
    *size = CONSOLE_WIN_SIZE;
}

// Panic-path reclaim (console.cc): force the console UART back to a polled-ready 8N1 TX
// channel after a userspace driver may have garbled every writable register in the window.
// Runs with IRQs masked, privileged; MUST be idempotent and re-entrant, so it is straight-line
// ABSOLUTE stores only, no read-modify-write.
// Bytes a dead driver left queued in the TX FIFO are NOT dropped: clearing LCR_H.FEN does not
// physically empty the FIFO, it only makes the flags read as a 1-byte holding register
// (DS 4.2.3.2.5 / 12.1.3.2.5), so those words still shift out ahead of the dump.
// The pin mux and pads are outside the window (arch_pinmux_set refuses the console pins);
// RESETS and CLOCKS are reserved blocks.
void arch_console_reclaim(void)
{
    // Silence first: a stale TXIM storms the console IRQ through the dump. Absolute store, not
    // the CLR alias rp_tx_irq_disable uses, which would leave every other source as found.
    r32(reg::uart::IMSC) = 0;

    // Stop the UART before the LCR writes below: every LCR write must precede enabling the
    // UART (DS 4.2.7 / 12.1.7).
    r32(reg::uart::CR) = 0;

    // IBRD, FBRD, then LCR_H: the three are one internal 30-bit register that latches only on
    // the LCR_H write (DS 4.2.3.2 / 12.1.3.2), so this order is load-bearing.
    if (console_clk_on_pll())
    {
        r32(reg::uart::IBRD) = reg::uart::IBRD_PLL;
        r32(reg::uart::FBRD) = reg::uart::FBRD_PLL;
    }
    else
    {
        r32(reg::uart::IBRD) = reg::uart::IBRD_115200;
        r32(reg::uart::FBRD) = reg::uart::FBRD_115200;
    }
    // 8N1, FEN off. Also clears BRK, which pins UARTTXD low and sends nothing.
    r32(reg::uart::LCR_H) = reg::uart::LCR_H_8N1;

    r32(reg::uart::DMACR) = 0; // a DMA channel still armed on TXDMAE interleaves its own bytes
    r32(reg::uart::IFLS) = reg::uart::IFLS_RESET; // inert while FEN is off; not worth depending on that

    // LAST, and absolute: CTSEN gates every byte on a CTS this board does not wire, LBE feeds
    // UARTTXD back into UARTRXD, SIREN turns the pin into IrDA pulses.
    r32(reg::uart::CR) = reg::uart::CR_ENABLE;
}

// Console coherence (arch.h): block until the console UART is transmission-complete. FR.TXFF
// clear only means the holding register took the byte; FR.BUSY stays set until the last stop
// bit has left the shift register (DS 4.2.8 / 12.1.8, UARTFR).
void arch_console_flush_sync(void)
{
    uint32_t spin = 0;
    while ((r32(reg::uart::FR) & reg::uart::FR_BUSY) != 0)
    {
        if (++spin > KICKOS_POLL_SPIN_MAX)
        {
            return; // bounded, as arch.h requires: a wedged UART drops the tail, never hangs
        }
    }
}

// Monotonic clock from the 64-bit system timer (microseconds -> ns). Uses the non-latching RAW
// halves with a hi/lo/hi re-read to tolerate a 32-bit rollover between the reads, which stays
// correct on a second core (the latching TIMELR/TIMEHR pair is single-core only). Overrides the
// required per-chip clock: this counter is a true 64-bit source with no 32-bit wrap.
uint64_t arch_clock_now(void)
{
    uint32_t hi = r32(reg::timer::TIMERAWH);
    uint32_t lo;
    while (true)
    {
        lo = r32(reg::timer::TIMERAWL);
        uint32_t hi2 = r32(reg::timer::TIMERAWH);
        if (hi2 == hi)
        {
            break;
        }
        hi = hi2;
    }
    return ((static_cast<uint64_t>(hi) << 32) | lo) * 1000ull;
}

// Telemetry trace clock: the low 32 bits of the free-running 1 MHz system timer (us, wraps
// ~71 min). Same source as arch_clock_now (a single RAW-low read), so the SESSION-anchor rate
// is exactly 1000 ns/tick.
uint32_t arch_trace_now(void)
{
    return r32(reg::timer::TIMERAWL);
}

}
