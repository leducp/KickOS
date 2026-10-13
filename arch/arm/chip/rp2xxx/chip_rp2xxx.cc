// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Bring-up and console transport shared by the RP2040 and the RP2350 (datasheets RP-008371-DS
// and RP-008373-DS-2); a section number written "a / b" cites the RP2040 first.

#include <kickos/arch/arch.h>
#include <kickos/arch/bounded_wait.h>
#include <kickos/arch/console_poll.h>
#include <kickos/arch/console_retry.h>
#include <kickos/console_tx.h>
#include <kickos/sys/abi.h>

#include <stdint.h>

#include "board_pins.h"
#include "bringup_values.h"
#include "family_map.h"
#include "pin_guard.h"
#include "regs/clocks.h"
#include "regs/io_bank0.h"
#include "regs/pads.h"
#include "regs/pll.h"
#include "regs/resets.h"
#include "regs/sio.h"
#include "regs/timer.h"
#include "regs/uart.h"
#include "regs/xosc.h"
#include "rp2xxx.h"

// clk_sys in Hz, which SysTick reads live.
extern "C" uint32_t SystemCoreClock;

namespace reg = kickos::rp2xxx::reg;

#if KICKOS_AMP_OWN_IMAGE
namespace
{
    using kickos::console_retry::Put;

    static_assert(KICKOS_KERNEL_CORES == 1, "one polled writer state serves every caller");
    kickos::console_retry::PolledLine g_polled;

    // One byte under the partition claim, checked after the FIFO wait: the grant can end
    // while a byte waits for room.
    Put console_put(char c)
    {
        if (not kickos::arch::wait_clear(r32(reg::uart::FR), reg::uart::FR_TXFF))
        {
            kickos::rp2xxx::console_drop(true); // a wedged channel keeps no claim
            return Put::WEDGED;
        }
        if (not kickos::rp2xxx::console_claim())
        {
            return Put::LOST;
        }
        r32(reg::uart::DR) = static_cast<uint8_t>(c);
        return Put::STORED;
    }
}
#else
namespace
{
    // Relies on the FIFO being off (LCR_H_8N1). The idle->busy prime assumes TXIM fires whether
    // level- or edge-triggered at rest (HW-unverified). IMSC goes through the SET/CLR aliases,
    // never a read-modify-write.
    int tx_slot_free(void)
    {
        return (r32(reg::uart::FR) & reg::uart::FR_TXFF) == 0;
    }
    void tx_push(uint8_t b)
    {
        r32(reg::uart::DR) = b;
    }
    void tx_irq_enable(void)
    {
        r32(reg::uart::IMSC + kickos::rp2xxx::ATOMIC_SET) = reg::uart::IMSC_TXIM;
    }
    void tx_irq_disable(void)
    {
        r32(reg::uart::IMSC + kickos::rp2xxx::ATOMIC_CLR) = reg::uart::IMSC_TXIM;
    }
}

console_tx_backend const kickos::rp2xxx::console_backend = {
    tx_slot_free, tx_push, tx_irq_enable, tx_irq_disable, &kickos_console_tx_storage};
#endif

namespace kickos::rp2xxx
{
    bool wait_mask(uintptr_t addr, uint32_t mask)
    {
        return kickos::arch::wait_set(r32(addr), mask);
    }

    void unreset(uint32_t mask)
    {
        r32(reg::resets::RESET + ATOMIC_CLR) = mask;
        wait_mask(reg::resets::RESET_DONE, mask);
    }

    // Bring PLL_SYS up to the chip's clk_sys target. Returns false (PLL left powered down) if
    // the VCO never locks, so the caller can stay on the crystal instead of switching clk_sys
    // onto a dead PLL. Datasheet 2.18.2 / 8.6.4 sequence.
    bool pll_sys_lock()
    {
        // Reset the block first so a warm reboot can't run this off stale dividers.
        r32(reg::resets::RESET + ATOMIC_SET) = reg::resets::PLL_SYS;
        r32(reg::resets::RESET + ATOMIC_CLR) = reg::resets::PLL_SYS;
        wait_mask(reg::resets::RESET_DONE, reg::resets::PLL_SYS);

        // Load REFDIV + FBDIV BEFORE powering the VCO.
        r32(reg::pll::CS) = reg::pll::CS_REFDIV_1;
        r32(reg::pll::FBDIV_INT) = reg::pll::FBDIV_125;
        // Power up main regulator + VCO (clear PD, VCOPD). DSMPD stays set (integer FBDIV, no
        // delta-sigma); POSTDIVPD stays set until after lock.
        r32(reg::pll::PWR + ATOMIC_CLR) = reg::pll::PWR_PD | reg::pll::PWR_VCOPD;
        if (not wait_mask(reg::pll::CS, reg::pll::CS_LOCK))
        {
            return false;
        }
        r32(reg::pll::PRIM) = reg::pll::PRIM_POSTDIV;
        r32(reg::pll::PWR + ATOMIC_CLR) = reg::pll::PWR_POSTDIVPD;
        return true;
    }

    void clocks_init()
    {
        r32(reg::xosc::STARTUP) = reg::xosc::STARTUP_DELAY;
        // Program the frequency range, THEN start the oscillator (datasheet 2.16 / 8.2.7): a
        // combined write could latch ENABLE before FREQ_RANGE.
        r32(reg::xosc::CTRL) = reg::xosc::FREQ_1_15MHZ;
        r32(reg::xosc::CTRL + ATOMIC_SET) = reg::xosc::ENABLE;

        bool xosc_ok = wait_mask(reg::xosc::STATUS, reg::xosc::STATUS_STABLE);
        if (xosc_ok)
        {
            r32(reg::clocks::CLK_REF_CTRL) = reg::clocks::CLK_REF_SRC_XOSC;
            xosc_ok = wait_mask(reg::clocks::CLK_REF_SELECTED, reg::clocks::CLK_REF_SELECTED_XOSC);
        }

        if (not xosc_ok)
        {
            SystemCoreClock = reg::clocks::ROSC_NOMINAL_HZ;
            r32(reg::clocks::CLK_PERI_CTRL) = reg::clocks::CLK_PERI_ENABLE_CLK_SYS;
            part::tick_start(tick_cycles(reg::clocks::CLK_REF_ROSC_NOMINAL_HZ));
            return;
        }

        // clk_ref stays on the crystal: the system timer's tick derives from it and MUST NOT
        // track the PLL.
        part::tick_start(tick_cycles(part::clk_ref_xosc_hz()));

        if (pll_sys_lock())
        {
            // The glitchless clk_sys mux (datasheet 2.15.3.1 / 8.1.3.2): AUXSRC while still on
            // clk_ref, then SRC to aux, each confirmed through SELECTED.
            r32(reg::clocks::CLK_SYS_CTRL) = reg::clocks::CLK_SYS_AUXSRC_PLL | reg::clocks::CLK_SYS_SRC_REF;
            wait_mask(reg::clocks::CLK_SYS_SELECTED, reg::clocks::CLK_SYS_SELECTED_REF);
            r32(reg::clocks::CLK_SYS_CTRL) = reg::clocks::CLK_SYS_AUXSRC_PLL | reg::clocks::CLK_SYS_SRC_AUX;
            wait_mask(reg::clocks::CLK_SYS_SELECTED, reg::clocks::CLK_SYS_SELECTED_AUX);
            SystemCoreClock = reg::clocks::CLK_SYS_HZ;
            r32(reg::clocks::CLK_PERI_CTRL) = reg::clocks::CLK_PERI_ENABLE_CLK_SYS;
        }
        else
        {
            SystemCoreClock = reg::clocks::CLK_SYS_NO_PLL_HZ;
            r32(reg::clocks::CLK_PERI_CTRL) = reg::clocks::CLK_PERI_ENABLE_XOSC;
        }
    }

    // Refused below CLK_SYS_MIN_FOR_USB_HZ: full-speed USB cannot run off the ROSC, and a slow
    // clk_sys breaks the clk_sys > 1.1 x clk_usb workaround for RP2040-E16 / RP2350-E12. Silent,
    // the console not being up yet: the driver finds the block still in reset.
    void usb_clock_init()
    {
        if (SystemCoreClock < reg::clocks::CLK_SYS_MIN_FOR_USB_HZ)
        {
            return;
        }
        r32(reg::resets::RESET + ATOMIC_SET) = reg::resets::PLL_USB;
        r32(reg::resets::RESET + ATOMIC_CLR) = reg::resets::PLL_USB;
        wait_mask(reg::resets::RESET_DONE, reg::resets::PLL_USB);

        r32(reg::pll_usb::CS) = reg::pll::CS_REFDIV_1;
        r32(reg::pll_usb::FBDIV_INT) = reg::pll_usb::FBDIV_100;
        r32(reg::pll_usb::PWR + ATOMIC_CLR) = reg::pll::PWR_PD | reg::pll::PWR_VCOPD;
        if (not wait_mask(reg::pll_usb::CS, reg::pll::CS_LOCK))
        {
            return;
        }
        r32(reg::pll_usb::PRIM) = reg::pll_usb::PRIM_POSTDIV;
        r32(reg::pll_usb::PWR + ATOMIC_CLR) = reg::pll::PWR_POSTDIVPD;

        // clk_usb has no glitchless mux, so its source changes only with the generator stopped
        // (datasheet 2.15.3.2 / 8.1.3.2).
        r32(reg::clocks::CLK_USB_CTRL) = reg::clocks::CLK_USB_AUXSRC_PLL_USB;
        part::clk_usb_stop_wait();
        r32(reg::clocks::CLK_USB_CTRL) =
            reg::clocks::CLK_USB_AUXSRC_PLL_USB | reg::clocks::CLK_USB_ENABLE;

        // RESET_DONE asserts only once clk_usb runs, and the DPRAM is unreachable until then.
        unreset(reg::resets::USBCTRL);
    }

    // One store clears the pad's release bits and output disable, ahead of its input enable.
    void pin_function(uint32_t pin, uint32_t func)
    {
        r32(reg::io_bank0::gpio_ctrl(pin)) = func & PIN_FUNCSEL_MASK;
        uint32_t const clr = pad_clear(func, part::PAD_RELEASE, reg::pads::OD);
        if (clr != 0u)
        {
            r32(reg::pads::gpio(pin) + ATOMIC_CLR) = clr;
        }
        if ((func & PIN_INPUT) != 0u)
        {
            r32(reg::pads::gpio(pin) + ATOMIC_SET) = reg::pads::IE;
        }
        if ((func & PIN_SIO_OUT) != 0u)
        {
            r32(reg::sio::GPIO_OE_SET) = 1u << pin;
        }
    }
}

namespace
{
    // The window arch_console_reclaim rewrites: the console UART's whole APB slot, which is
    // the register block plus the XOR/SET/CLR aliases at +0x1000/+0x2000/+0x3000. The aliases
    // must be inside it: a holder granted only an alias writes the very same registers.
    constexpr uintptr_t CONSOLE_WIN_BASE = reg::uart::BASE;
    constexpr size_t CONSOLE_WIN_SIZE = KICKOS_BOARD_CONSOLE_SIZE;

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

    // IBRD, FBRD, then LCR_H: the three are one internal register that latches only on the LCR_H
    // write (DS 4.2.3.2 / 12.1.3.2). 8N1, FEN off.
    void uart_frame_set()
    {
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
        r32(reg::uart::LCR_H) = reg::uart::LCR_H_8N1;
    }

    static_assert(KICKOS_BOARD_CONSOLE_BASE == reg::uart::BASE,
                  "the board's console is not the UART this backend drives");
    static_assert((KICKOS_BOARD_CONSOLE_TX_SELECT & ~kickos::rp2xxx::PIN_FUNCSEL_MASK) == 0u
                      and (KICKOS_BOARD_CONSOLE_RX_SELECT & ~kickos::rp2xxx::PIN_FUNCSEL_MASK) == 0u,
                  "a console pin's function is wider than the funcsel field");
}

namespace kickos::rp2xxx
{
    // After clocks_init, whose clock tree picks the divisors.
    void console_uart_init()
    {
        pin_function(KICKOS_BOARD_CONSOLE_TX_BIT, KICKOS_BOARD_CONSOLE_TX_SELECT | PIN_DRIVE);
        pin_function(KICKOS_BOARD_CONSOLE_RX_BIT, KICKOS_BOARD_CONSOLE_RX_SELECT | PIN_INPUT);
        uart_frame_set();
        r32(reg::uart::IMSC) = 0; // the ring arms TXIM
        r32(reg::uart::CR) = reg::uart::CR_ENABLE;
    }
}

extern "C"
{

#if KICKOS_AMP_OWN_IMAGE
int arch_console_write_retry(char const* buf, size_t n, bool* cr_pending)
{
    if (n == 0 or not kickos::rp2xxx::console_claim_open(cr_pending == nullptr))
    {
        return 0;
    }
    // Masked as console_write_line_sync is, so a local ISR's output cannot land inside the
    // line. Stops at the first byte the claim no longer covers: the count is what went out,
    // and the rest is the caller's to offer again under a fresh claim.
    arch_irq_state_t const irq = arch_irq_save();
    bool const had_pending_cr = cr_pending != nullptr and *cr_pending;
    size_t const landed = kickos::console_retry::write(
        buf, n, cr_pending, [](char c) { return console_put(c) == Put::STORED; });
    bool const ended_line = (landed != 0 and buf[landed - 1] == '\n') or
                            (had_pending_cr and cr_pending != nullptr and not *cr_pending and
                             landed == 0);
    kickos::rp2xxx::console_drop(ended_line);
    arch_irq_restore(irq);
    return static_cast<int>(landed);
}
#endif

bool arch_console_write_sync(char const* buf, size_t n)
{
    if (n == 0)
    {
        return true;
    }
#if KICKOS_AMP_OWN_IMAGE
    bool const sent = g_polled.write(buf, n, console_put);
    kickos::rp2xxx::console_drop(buf[n - 1] == '\n');
    return sent;
#else
    return kickos::console_poll::write<tx_slot_free, tx_push>(buf, n);
#endif
}

void arch_console_reclaim_window(uintptr_t* base, size_t* size)
{
    *base = CONSOLE_WIN_BASE;
    *size = CONSOLE_WIN_SIZE;
}

// Panic-path reclaim after a userspace driver may have garbled every writable register in the
// window. Runs privileged with IRQs masked; MUST be idempotent and re-entrant: straight-line
// ABSOLUTE stores, no read-modify-write.
// Bytes a dead driver left queued in the TX FIFO are NOT dropped: clearing LCR_H.FEN only makes
// the flags read as a 1-byte holding register (DS 4.2.3.2.5 / 12.1.3.2.5), so they still shift
// out ahead of the dump. The pin mux and pads are outside the window (arch_pinmux_set refuses
// the console pins); RESETS and CLOCKS are reserved blocks.
void arch_console_reclaim(void)
{
    // Silence first: a stale TXIM storms the console IRQ through the dump. Absolute store, not
    // the CLR alias rp_tx_irq_disable uses, which would leave every other source as found.
    r32(reg::uart::IMSC) = 0;

    // Stop the UART before the LCR writes below: every LCR write must precede enabling the
    // UART (DS 4.2.7 / 12.1.7).
    r32(reg::uart::CR) = 0;

    // Also clears BRK, which pins UARTTXD low and sends nothing.
    uart_frame_set();

    r32(reg::uart::DMACR) = 0; // a DMA channel still armed on TXDMAE interleaves its own bytes
    r32(reg::uart::IFLS) = reg::uart::IFLS_RESET; // inert while FEN is off; not worth depending on that

    // LAST, and absolute: CTSEN gates every byte on a CTS this board does not wire, LBE feeds
    // UARTTXD back into UARTRXD, SIREN turns the pin into IrDA pulses.
    r32(reg::uart::CR) = reg::uart::CR_ENABLE;
}

// Blocks until the console UART is transmission-complete: FR.TXFF clear only means the holding
// register took the byte, FR.BUSY waits for the last stop bit (DS 4.2.8 / 12.1.8).
void arch_console_flush_sync(void)
{
    (void)kickos::arch::wait_clear(r32(reg::uart::FR), reg::uart::FR_BUSY);
}

// Monotonic clock from the 64-bit system timer. The non-latching RAW halves are read hi/lo/hi to
// tolerate a rollover between the reads, which stays correct on a second core (the latching
// TIMELR/TIMEHR pair is single-core only).
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

// One-shot pin-function config (KOS_SYS_PINMUX_SET), `func` packed as bringup_values.h states.
// A peripheral input needs PIN_INPUT: the RP2350's input enable resets 0.
int arch_pinmux_set(uint32_t port, uint32_t pin, uint32_t func)
{
    if (port != 0u or pin > 29u)
    {
        return -KOS_EINVAL;
    }
    if (kickos::board_pin_kernel_owned(port, pin))
    {
        return -KOS_EBUSY;
    }
    kickos::rp2xxx::pin_function(pin, func);
    return 0;
}

// Telemetry trace clock: the low 32 bits of the free-running 1 MHz system timer (us, wraps
// ~71 min). Same source as arch_clock_now (a single RAW-low read), so the SESSION-anchor rate
// is exactly 1000 ns/tick.
uint32_t arch_trace_now(void)
{
    return r32(reg::timer::TIMERAWL);
}

}
