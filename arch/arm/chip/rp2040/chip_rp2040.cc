// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Raspberry Pi RP2040 (Pico), Cortex-M0+ chip backend. Register addresses/fields
// are clean-room from the RP2040 datasheet (RP-008371-DS); hand-rolled, no vendor
// SDK sources, consistent with the arch layer's regs.h.
//
// M1 scope: privilege + SVC, no hardware MPU. clk_sys is raised to 125 MHz off
// PLL_SYS (12 MHz XOSC x125 /6 /2); SystemCoreClock tracks it so the SysTick
// ns<->cycle math (arch_arm_common) stays coherent. clk_ref stays on the 12 MHz
// XOSC and the WATCHDOG /12 tick is untouched, so the 1 MHz system TIMER
// (arch_clock_now / arch_trace_now; arch.h requires a 64-bit monotonic clock and
// v6-M has no DWT) is PLL-independent. clk_peri follows clk_sys to 125 MHz, so the
// UART baud divisors are recomputed for 125 MHz (uart0_init). If the crystal or the
// PLL never comes up the board degrades to XOSC/ROSC timing instead of hanging.
//
// NOT run in this environment (no RP2040 model in mainline QEMU); verified by
// build + image inspection. Flash to a Pico (drag the UF2/.bin via BOOTSEL, or
// SWD) to confirm UART0 output on GP0 (pin 1). The board can always be recovered
// via BOOTSEL, so a wrong boot2/clock config cannot permanently brick it.
//
// The second-stage bootloader (boot2.S) and its CRC wrapper run BEFORE this file;
// by the time Reset_Handler executes, code is already executing in place from
// flash. See boot2.S and cmake/rp2040_checksum.py.

#include <kickos/arch/arch.h>
#include <kickos/config/limits.h>
#include <kickos/console_tx.h>
#include <kickos/sys/abi.h> // KOS_E* taxonomy (arch_pinmux_set)

#include <stdint.h>

// Hand-rolled register map for this chip (clean-room, no vendor SDK). Bases in
// mmap.h, NVIC lines in irq.h, per-peripheral offsets/fields in regs/; the atomic
// SET/CLR/XOR alias helpers in regs/atomic.h.
#include "mmap.h"
#include "irq.h"
#include "regs/atomic.h"
#include "regs/clocks.h"
#include "regs/io_bank0.h"
#include "regs/pads.h"
#include "regs/pll.h"
#include "regs/resets.h"
#include "regs/sio.h"
#include "regs/timer.h"
#include "regs/uart.h"
#include "regs/watchdog.h"
#include "regs/xosc.h"

namespace mmap = kickos::rp2040::mmap;
namespace reg = kickos::rp2040::reg;
namespace irq = kickos::rp2040::irq;

namespace kickos
{
    int kmain(int argc, char** argv);
}

extern "C"
{
    void kickos_armv6m_init(void);

    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();

    // Pre-init value (12 MHz XOSC, reset). clocks_init() raises this to 125 MHz
    // once clk_sys is on PLL_SYS; SysTick (processor clock) reads it live.
    uint32_t SystemCoreClock = 12000000u;
}

namespace
{
    inline volatile uint32_t& r32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }

    // Chosen by clocks_init (which source clk_peri lands on), consumed by
    // uart0_init. Boot is single-threaded and sequential, so no guard is needed.
    uint32_t g_uart_ibrd = reg::uart::IBRD_115200;
    uint32_t g_uart_fbrd = reg::uart::FBRD_115200;

    // Bounded so a dead/missing crystal or stuck peripheral degrades instead of
    // hanging the boot forever (a silent hang leaves no LED/UART sign of life).
    // The cap is far longer than any legitimate wait (XOSC startup is ~1 ms).
    constexpr uint32_t POLL_TIMEOUT = 1000000u;

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
        r32(reg::atomic::as_clr(reg::resets::RESET)) = mask;
        wait_mask(reg::resets::RESET_DONE, mask); // bounded; best-effort
    }

    // Bring PLL_SYS up to 125 MHz. Returns false (PLL left powered down) if the VCO
    // never locks, so the caller can stay on the crystal instead of switching
    // clk_sys onto a dead PLL. Datasheet 2.18.2 sequence.
    bool pll_sys_lock()
    {
        // Reset the block first so a warm reboot can't run this off stale dividers.
        r32(reg::atomic::as_set(reg::resets::RESET)) = reg::resets::PLL_SYS;
        r32(reg::atomic::as_clr(reg::resets::RESET)) = reg::resets::PLL_SYS;
        wait_mask(reg::resets::RESET_DONE, reg::resets::PLL_SYS);

        // Load REFDIV + FBDIV BEFORE powering the VCO.
        r32(reg::pll::CS) = reg::pll::CS_REFDIV_1;
        r32(reg::pll::FBDIV_INT) = reg::pll::FBDIV_125;
        // Power up main regulator + VCO (clear PD, VCOPD). DSMPD stays set (integer
        // FBDIV, no delta-sigma); POSTDIVPD stays set until after lock.
        r32(reg::atomic::as_clr(reg::pll::PWR)) = reg::pll::PWR_PD | reg::pll::PWR_VCOPD;
        if (not wait_mask(reg::pll::CS, reg::pll::CS_LOCK))
        {
            return false;
        }
        r32(reg::pll::PRIM) = reg::pll::PRIM_POSTDIV;
        r32(reg::atomic::as_clr(reg::pll::PWR)) = reg::pll::PWR_POSTDIVPD; // enable post-dividers
        return true;
    }

    void clocks_init()
    {
        // Bring up the 12 MHz crystal and put clk_ref on it. If it never stabilizes,
        // degrade to the ROSC that clk_sys already runs on at reset so the board
        // still boots (approximate timing) instead of hanging.
        r32(reg::xosc::STARTUP) = reg::xosc::STARTUP_DELAY;
        // Program the frequency range, THEN start the oscillator (datasheet
        // sequence): a combined write is avoided so ENABLE never latches before
        // FREQ_RANGE is in place.
        r32(reg::xosc::CTRL) = reg::xosc::FREQ_1_15MHZ;
        r32(reg::atomic::as_set(reg::xosc::CTRL)) = reg::xosc::ENABLE;

        bool xosc_ok = wait_mask(reg::xosc::STATUS, reg::xosc::STATUS_STABLE);
        if (xosc_ok)
        {
            // clk_ref <- XOSC (glitchless mux); clk_sys follows to 12 MHz via its
            // SRC=clk_ref reset default. Poll the one-hot SELECTED before proceeding.
            r32(reg::clocks::CLK_REF_CTRL) = reg::clocks::CLK_REF_SRC_XOSC;
            xosc_ok = wait_mask(reg::clocks::CLK_REF_SELECTED, reg::clocks::CLK_REF_SELECTED_XOSC);
        }

        if (not xosc_ok)
        {
            SystemCoreClock = reg::xosc::ROSC_NOMINAL_HZ;              // clk_sys stayed on ROSC
            r32(reg::clocks::CLK_PERI_CTRL) = reg::clocks::CLK_PERI_ENABLE_CLK_SYS; // UART clock <- clk_sys
            r32(reg::watchdog::TICK) = reg::watchdog::TICK_CFG_ROSC;   // ~6.5 MHz / 7 ~= 1 MHz
            return;
        }

        // clk_ref stays on the 12 MHz XOSC: the WATCHDOG /12 tick and thus the 1 MHz
        // system TIMER (arch_clock_now / arch_trace_now) derive from clk_ref and MUST
        // NOT track the PLL.
        r32(reg::watchdog::TICK) = reg::watchdog::TICK_CFG; // 12 MHz / 12 = 1 MHz tick

        if (pll_sys_lock())
        {
            // Switch the clk_sys glitchless mux onto the PLL (datasheet 2.15.3.1):
            // set AUXSRC while still on clk_ref, then flip SRC to aux and poll SELECTED.
            r32(reg::clocks::CLK_SYS_CTRL) = reg::clocks::CLK_SYS_AUXSRC_PLL | reg::clocks::CLK_SYS_SRC_REF;
            wait_mask(reg::clocks::CLK_SYS_SELECTED, reg::clocks::CLK_SYS_SELECTED_REF);
            r32(reg::clocks::CLK_SYS_CTRL) = reg::clocks::CLK_SYS_AUXSRC_PLL | reg::clocks::CLK_SYS_SRC_AUX;
            wait_mask(reg::clocks::CLK_SYS_SELECTED, reg::clocks::CLK_SYS_SELECTED_AUX);
            // CLK_SYS_DIV stays at its reset value (INT=1, /1). Update the core-clock
            // truth in the SAME step (arch_arm_common SysTick reads SystemCoreClock).
            SystemCoreClock = reg::clocks::CLK_SYS_HZ;
            g_uart_ibrd = reg::uart::IBRD_125MHZ;
            g_uart_fbrd = reg::uart::FBRD_125MHZ;
            r32(reg::clocks::CLK_PERI_CTRL) = reg::clocks::CLK_PERI_ENABLE_CLK_SYS; // UART clock <- clk_sys 125 MHz
        }
        else
        {
            // PLL never locked: clk_sys still follows clk_ref (12 MHz). SystemCoreClock
            // and the UART divisors keep their 12 MHz defaults.
            r32(reg::clocks::CLK_PERI_CTRL) = reg::clocks::CLK_PERI_ENABLE_XOSC; // UART clock <- XOSC 12 MHz
        }
    }

    void uart0_init()
    {
        // Route GP0/GP1 to UART0 and make the pads usable (TX drives out, RX in).
        r32(reg::io_bank0::GPIO0_CTRL) = reg::io_bank0::FUNCSEL_UART;
        r32(reg::io_bank0::GPIO1_CTRL) = reg::io_bank0::FUNCSEL_UART;
        r32(reg::atomic::as_clr(reg::pads::GPIO0)) = reg::pads::OD;
        r32(reg::atomic::as_set(reg::pads::GPIO1)) = reg::pads::IE;

        // Divisors latch only on the subsequent LCR_H write, so order matters.
        r32(reg::uart::IBRD) = g_uart_ibrd;
        r32(reg::uart::FBRD) = g_uart_fbrd;
        r32(reg::uart::LCR_H) = reg::uart::LCR_H_8N1;
        r32(reg::uart::IMSC) = 0; // all UART interrupt sources masked; the ring arms TXIM
        r32(reg::uart::CR) = reg::uart::CR_ENABLE;
    }

    // --- Buffered console TX backend (console_tx.h). The ring drains via the PL011
    // transmit interrupt with the FIFO disabled (see LCR_H_8N1); the idle->busy
    // prime starts the transfer whether TXIM is level- or transition-triggered at
    // rest (HW-unverified). slot_free/push touch one data register;
    // irq_enable/disable use the RP2040 atomic set/clear aliases so no read-modify-
    // write on IMSC is needed. ---
    int rp_tx_slot_free(void) { return (r32(reg::uart::FR) & reg::uart::FR_TXFF) == 0; }
    void rp_tx_push(uint8_t b) { r32(reg::uart::DR) = b; }
    void rp_tx_irq_enable(void) { r32(reg::atomic::as_set(reg::uart::IMSC)) = reg::uart::IMSC_TXIM; }
    void rp_tx_irq_disable(void) { r32(reg::atomic::as_clr(reg::uart::IMSC)) = reg::uart::IMSC_TXIM; }

    constexpr uint32_t CONSOLE_TX_SIZE = 512; // power of two; > kprintf's 256B buffer
    char console_tx_buf[CONSOLE_TX_SIZE];
    console_tx_backend const rp_console_backend = {
        rp_tx_slot_free, rp_tx_push, rp_tx_irq_enable, rp_tx_irq_disable};
}

extern "C"
{

void arch_init(void)
{
    // Reset-release ordering is load-bearing: a peripheral's RESET_DONE only
    // asserts once it has a running clock. IO_BANK0/PADS_BANK0/TIMER are clocked
    // by clk_sys/clk_ref (already live off the ROSC at reset), so release them
    // now. UART0 is clocked by clk_peri, which is OFF until clocks_init -- release
    // it BEFORE that and its RESET_DONE never asserts, hanging the boot.
    unreset(reg::resets::IO_BANK0 | reg::resets::PADS_BANK0 | reg::resets::TIMER);
    clocks_init();
    unreset(reg::resets::UART0);
    uart0_init();
    kickos_armv6m_init();
}

void arch_console_write(char const* buf, size_t n)
{
    console_tx_write(buf, n); // buffered; the routing guard (console.cc) keeps this thread-only
}

void arch_console_write_sync(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        uint32_t spin = 0;
        while ((r32(reg::uart::FR) & reg::uart::FR_TXFF) != 0)
        {
            if (++spin > KICKOS_POLL_SPIN_MAX)
            {
                return; // bounded: a wedged UART must not hang the panic path (drop)
            }
        }
        r32(reg::uart::DR) = static_cast<uint8_t>(buf[i]);
    }
}

console_tx_backend const* arch_console_tx_backend(char** storage, uint32_t* size, int* irq_line)
{
    *storage = console_tx_buf;
    *size = CONSOLE_TX_SIZE;
    *irq_line = irq::UART0_IRQ;
    return &rp_console_backend;
}

// Kernel diagnostic LED: GP25 via SIO, active-high (NOT the Pico W CYW43 LED).
void arch_diag_led_init(void)
{
    r32(reg::io_bank0::GPIO25_CTRL) = reg::io_bank0::FUNCSEL_SIO;   // funcsel = SIO
    r32(reg::atomic::as_clr(reg::pads::GPIO25)) = reg::pads::OD;    // clear output-disable
    r32(reg::sio::GPIO_OE_SET) = 1u << 25;                         // output enable
}

void arch_diag_led_set(int on)
{
    if (on)
    {
        r32(reg::sio::GPIO_OUT_SET) = 1u << 25;
    }
    else
    {
        r32(reg::sio::GPIO_OUT_CLR) = 1u << 25;
    }
}

// Kernel-owned pins arch_pinmux_set refuses so a board map cannot dark the console
// or steal the diag LED. GP0/GP1 = UART0 TX/RX; GP25 = diag LED via SIO.
static bool rp2040_pin_kernel_owned(uint32_t pin)
{
    return pin == 0u or pin == 1u or pin == 25u;
}

// One-shot pin-function config (KOS_SYS_PINMUX_SET). func packs the IO_BANK0 CTRL
// funcsel in bits[4:0] plus pad/SIO side effects: bit[8] set pad IE, bit[9] clear
// pad OD (drive out), bit[16] enable the SIO output (GPIO_OE_SET, 1<<pin). IE resets
// 1 here, so bit[8] is belt-and-braces. IO_BANK0/PADS are already unreset+clocked
// from arch_init, so no clock gate is needed.
int arch_pinmux_set(uint32_t port, uint32_t pin, uint32_t func)
{
    if (port != 0u or pin > 29u)
    {
        return -KOS_EINVAL;
    }
    if (rp2040_pin_kernel_owned(pin))
    {
        return -KOS_EBUSY;
    }
    r32(reg::io_bank0::gpio_ctrl(pin)) = func & 0x1fu;
    if ((func & (1u << 8)) != 0u)
    {
        r32(reg::atomic::as_set(reg::pads::gpio(pin))) = reg::pads::IE;
    }
    if ((func & (1u << 9)) != 0u)
    {
        r32(reg::atomic::as_clr(reg::pads::gpio(pin))) = reg::pads::OD;
    }
    if ((func & (1u << 16)) != 0u)
    {
        r32(reg::sio::GPIO_OE_SET) = 1u << pin;
    }
    return 0;
}

// Monotonic clock from the 64-bit system TIMER (microseconds -> ns). Uses the
// non-latching RAW halves with a hi/lo/hi re-read to tolerate a 32-bit rollover
// between the reads. This needs no interrupt guard and stays correct if a future
// milestone launches core 1 (the latching TIMELR/TIMEHR pair is single-core only).
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

// Telemetry trace clock: the low 32 bits of the free-running 1 MHz system TIMER
// (us, wraps ~71 min). Same source as arch_clock_now (a single RAW-low read, no
// hi/lo guard needed for a u32), so the SESSION-anchor rate is exactly 1000 ns/tick.
uint32_t arch_trace_now(void)
{
    return r32(reg::timer::TIMERAWL);
}

#if KICKOS_HAVE_MPU
// Rule 7 reserved set (RP2040 datasheet). Owns-for-life: the 64-bit TIMER (monotonic
// base), the WATCHDOG (its /12 TICK feeds the 1 MHz TIMER -- reserved despite the
// general watchdog-exclusion, R3), and the RESETS + CLOCKS control blocks. Each is a
// full 16 KB window so the SET/CLR/XOR atomic aliases (+0x1000/+0x2000/+0x3000) are
// covered too (R2). M0+ has no bit-band -> weak arch_bitband_present 0.
size_t arch_reserved_blocks(struct arch_reserved_block* out, size_t max)
{
    static struct arch_reserved_block const blocks[] = {
        {mmap::TIMER_BASE, 0x4000u},    // TIMER: 64-bit us monotonic (DS 4.6)
        {mmap::WATCHDOG_BASE, 0x4000u}, // WATCHDOG: TICK generator for the TIMER (DS 4.7, R3)
        {mmap::RESETS_BASE, 0x4000u},   // RESETS: peripheral reset control (DS 2.14)
        {mmap::CLOCKS_BASE, 0x4000u},   // CLOCKS: clock generators (DS 2.15)
    };
    size_t n = sizeof(blocks) / sizeof(blocks[0]);
    if (n > max)
    {
        n = max;
    }
    for (size_t i = 0; i < n; i++)
    {
        out[i] = blocks[i];
    }
    return n;
}
#endif

void Reset_Handler(void)
{
    // Cortex-M0+ has no FPU; nothing to enable before the C runtime.
    kickos_ranges_init(); // init .data + the pow2 app-data block; zero .bss + app-bss
    for (void (**fn)() = __init_array_start; fn != __init_array_end; fn++)
    {
        (*fn)();
    }
    arch_init();
    kickos::kmain(0, nullptr);
    arch_shutdown(0);
}

}
