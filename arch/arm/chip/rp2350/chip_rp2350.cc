// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Raspberry Pi RP2350 (Cortex-M33) chip backend. Register addresses/fields are
// clean-room from the RP2350 datasheet (RP-008373-DS-2); hand-rolled, no vendor
// SDK sources, consistent with the arch layer's regs.h. Section numbers in the
// comments cite that datasheet.
//
// First-pass scope: privilege + SVC on the reused armv7m arch, NO hardware MPU
// (the armv8-m/PMSAv8 backend is deferred -- see docs/design-rp2350.md). clk_sys
// is raised to 150 MHz off PLL_SYS (12 MHz XOSC x125 /5 /2, the datasheet default
// max, 8.6); SystemCoreClock tracks it so the SysTick ns<->cycle math
// (arch_arm_common) stays coherent. clk_ref stays on the 12 MHz XOSC and drives
// the TICKS TIMER0 generator (/12 -> 1 MHz), so the 64-bit system TIMER0
// (arch_clock_now / arch_trace_now) is PLL-independent. clk_peri follows clk_sys,
// so the UART baud divisors are recomputed for 150 MHz. If the crystal or the PLL
// never comes up the board degrades to XOSC/ROSC timing instead of hanging.
//
// Key deltas from the RP2040 (all APB peripheral bases relocated; datasheet 2.2.4):
//   - No boot2/CRC stage: the bootrom does XIP setup + reads SP/PC from the vector
//     table (startup.S / rp2350.ld).
//   - The system TIMER tick comes from the new common TICKS block (8.5), not the
//     watchdog.
//   - PADS gained an ISO (isolation) bit that resets SET and must be cleared to use
//     a pad (9.11.3).
//   - 52 NVIC lines; the console is on UART1 (UART1_IRQ = 34, 3.2) -- see the
//     IO_BANK0 block below for why the Pi-Zero header forces UART1, not UART0.
//
// NOT run in this environment (no RP2350 model in mainline QEMU; no bench access);
// verified by build + image inspection. Flash via BOOTSEL/picotool to confirm UART1
// output on GP4 (Waveshare RP2350-Pi-Zero 40-pin header pin 8). The board is always
// BOOTSEL-recoverable, so a wrong
// clock/boot config cannot permanently brick it.

#include <kickos/arch/arch.h>
#include <kickos/config/limits.h>
#include <kickos/console_tx.h>

#include <stdint.h>

// Hand-rolled register map for this chip (clean-room, no vendor SDK).
// Bases in mmap.h, NVIC lines in irq.h, per-peripheral offsets/fields in regs/.
#include "mmap.h"
#include "irq.h"
#include "regs/clocks.h"
#include "regs/io_bank0.h"
#include "regs/pads.h"
#include "regs/pll.h"
#include "regs/resets.h"
#include "regs/ticks.h"
#include "regs/timer.h"
#include "regs/uart.h"
#include "regs/xosc.h"

namespace mmap = kickos::rp2350::mmap;
namespace reg = kickos::rp2350::reg;
namespace irq = kickos::rp2350::irq;

namespace kickos
{
    int kmain(int argc, char** argv);
}

extern "C"
{
void kickos_armv7m_init(void);
#if KICKOS_HAVE_MPU
// PMSAv8 MPU backend (arch/arm/common/arch_arm_pmsav8.cc): one-time MAIR + MemManage
// enable. This reference is also the LINK ANCHOR that pulls the PMSAv8 member so its
// strong kickos_arch_mpu_commit / arch_mpu_region_encodable win over the weak v7-M defs.
void kickos_arm_pmsav8_init(void);
#endif

extern void (*__init_array_start[])();
extern void (*__init_array_end[])();

// Pre-init value (12 MHz XOSC, reset). clocks_init() raises this to 150 MHz
// once clk_sys is on PLL_SYS; SysTick (processor clock) reads it live.
uint32_t SystemCoreClock = 12000000u;
}

namespace
{
    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }

    // Chosen by clocks_init (which source clk_peri lands on), consumed by uart1_init.
    // Boot is single-threaded and sequential, so no guard is needed.
    uint32_t g_uart_ibrd = reg::uart::IBRD_115200;
    uint32_t g_uart_fbrd = reg::uart::FBRD_115200;

    // Bounded so a dead/missing crystal or stuck peripheral degrades instead of
    // hanging the boot forever. The cap is far longer than any legitimate wait.
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
        r32(reg::resets::RESET + mmap::ATOMIC_CLR) = mask;
        wait_mask(reg::resets::RESET_DONE, mask); // bounded; best-effort
    }

    // Bring PLL_SYS up to 150 MHz. Returns false (PLL left powered down) if the VCO
    // never locks, so the caller can stay on the crystal instead of switching
    // clk_sys onto a dead PLL. Datasheet 8.6.4 sequence.
    bool pll_sys_lock()
    {
        // Reset the block first so a warm reboot can't run this off stale dividers.
        r32(reg::resets::RESET + mmap::ATOMIC_SET) = reg::resets::PLL_SYS;
        r32(reg::resets::RESET + mmap::ATOMIC_CLR) = reg::resets::PLL_SYS;
        wait_mask(reg::resets::RESET_DONE, reg::resets::PLL_SYS);

        // Load REFDIV + FBDIV BEFORE powering the VCO.
        r32(reg::pll::CS) = reg::pll::CS_REFDIV_1;
        r32(reg::pll::FBDIV_INT) = reg::pll::FBDIV_125;
        // Power up main regulator + VCO (clear PD, VCOPD). DSMPD stays set (integer
        // FBDIV, no delta-sigma); POSTDIVPD stays set until after lock.
        r32(reg::pll::PWR + mmap::ATOMIC_CLR) = reg::pll::PWR_PD | reg::pll::PWR_VCOPD;
        if (not wait_mask(reg::pll::CS, reg::pll::CS_LOCK))
        {
            return false;
        }
        r32(reg::pll::PRIM) = reg::pll::PRIM_POSTDIV;
        r32(reg::pll::PWR + mmap::ATOMIC_CLR) = reg::pll::PWR_POSTDIVPD; // enable post-dividers
        return true;
    }

    // Start the TICKS TIMER0 generator so the 64-bit system TIMER0 counts. The
    // generator must be stopped before CYCLES is changed (datasheet 8.5.1).
    void ticks_timer0_start(uint32_t cycles)
    {
        r32(reg::ticks::TIMER0_CTRL) = 0; // disable while reprogramming
        r32(reg::ticks::TIMER0_CYCLES) = cycles;
        r32(reg::ticks::TIMER0_CTRL) = reg::ticks::CTRL_ENABLE;
    }

    void clocks_init()
    {
        // Bring up the 12 MHz crystal and put clk_ref on it. If it never stabilizes,
        // degrade to the ROSC that clk_sys already runs on at reset so the board still
        // boots (approximate timing) instead of hanging.
        r32(reg::xosc::STARTUP) = reg::xosc::STARTUP_DELAY;
        // Program the frequency range, THEN start the oscillator (datasheet 8.2.7): a
        // combined write is avoided so ENABLE never latches before FREQ_RANGE is set.
        r32(reg::xosc::CTRL) = reg::xosc::FREQ_1_15MHZ;
        r32(reg::xosc::CTRL + mmap::ATOMIC_SET) = reg::xosc::ENABLE;

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
            SystemCoreClock = reg::clocks::ROSC_NOMINAL_HZ;            // clk_sys stayed on ROSC
            r32(reg::clocks::CLK_PERI_CTRL) = reg::clocks::CLK_PERI_ENABLE_CLK_SYS; // UART clock <- clk_sys
            ticks_timer0_start(reg::ticks::CYCLES_ROSC);              // ~6.5 MHz / 7 ~= 1 MHz
            return;
        }

        // clk_ref stays on the 12 MHz XOSC: the TICKS TIMER0 /12 tick and thus the
        // 1 MHz system TIMER0 (arch_clock_now / arch_trace_now) derive from clk_ref
        // and MUST NOT track the PLL.
        ticks_timer0_start(reg::ticks::CYCLES_12MHZ); // 12 MHz / 12 = 1 MHz tick

        if (pll_sys_lock())
        {
            // Switch the clk_sys glitchless mux onto the PLL (datasheet 8.1.3.2): set
            // AUXSRC while still on clk_ref, then flip SRC to aux and poll SELECTED.
            r32(reg::clocks::CLK_SYS_CTRL) = reg::clocks::CLK_SYS_AUXSRC_PLL | reg::clocks::CLK_SYS_SRC_REF;
            wait_mask(reg::clocks::CLK_SYS_SELECTED, reg::clocks::CLK_SYS_SELECTED_REF);
            r32(reg::clocks::CLK_SYS_CTRL) = reg::clocks::CLK_SYS_AUXSRC_PLL | reg::clocks::CLK_SYS_SRC_AUX;
            wait_mask(reg::clocks::CLK_SYS_SELECTED, reg::clocks::CLK_SYS_SELECTED_AUX);
            // CLK_SYS_DIV stays at its reset value (/1). Update the core-clock truth in
            // the SAME step (arch_arm_common SysTick reads SystemCoreClock).
            SystemCoreClock = reg::clocks::CLK_SYS_HZ;
            g_uart_ibrd = reg::uart::IBRD_150MHZ;
            g_uart_fbrd = reg::uart::FBRD_150MHZ;
            r32(reg::clocks::CLK_PERI_CTRL) = reg::clocks::CLK_PERI_ENABLE_CLK_SYS; // UART clock <- clk_sys 150 MHz
        }
        else
        {
            // PLL never locked: clk_sys still follows clk_ref (12 MHz). SystemCoreClock
            // and the UART divisors keep their 12 MHz defaults.
            r32(reg::clocks::CLK_PERI_CTRL) = reg::clocks::CLK_PERI_ENABLE_XOSC; // UART clock <- XOSC 12 MHz
        }
    }

    void uart1_init()
    {
        // Route GP4/GP5 to UART1 and make the pads usable. The RP2350 pads reset
        // ISOLATED (PAD_ISO set) -- clear it or the pad stays disconnected.
        r32(reg::io_bank0::GPIO4_CTRL) = reg::io_bank0::FUNCSEL_UART;
        r32(reg::io_bank0::GPIO5_CTRL) = reg::io_bank0::FUNCSEL_UART;
        r32(reg::pads::GPIO4 + mmap::ATOMIC_CLR) = reg::pads::ISO | reg::pads::OD; // TX: connect, drive out
        r32(reg::pads::GPIO5 + mmap::ATOMIC_CLR) = reg::pads::ISO;                 // RX: connect
        r32(reg::pads::GPIO5 + mmap::ATOMIC_SET) = reg::pads::IE;                  // RX: input enable

        // Divisors latch only on the subsequent LCR_H write, so order matters.
        r32(reg::uart::IBRD) = g_uart_ibrd;
        r32(reg::uart::FBRD) = g_uart_fbrd;
        r32(reg::uart::LCR_H) = reg::uart::LCR_H_8N1;
        r32(reg::uart::IMSC) = 0; // all UART interrupt sources masked; the ring arms TXIM
        r32(reg::uart::CR) = reg::uart::CR_ENABLE;
    }

    // --- Buffered console TX backend (console_tx.h). The ring drains via the PL011
    // transmit interrupt with the FIFO disabled (see LCR_H_8N1); the idle->busy prime
    // starts the transfer. slot_free/push touch one data register; irq_enable/disable
    // use the RP2350 atomic set/clear aliases so no read-modify-write on IMSC. ---
    int rp_tx_slot_free(void)
    {
        return (r32(reg::uart::FR) & reg::uart::FR_TXFF) == 0;
    }
    void rp_tx_push(uint8_t b)
    {
        r32(reg::uart::DR) = b;
    }
    void rp_tx_irq_enable(void)
    {
        r32(reg::uart::IMSC + mmap::ATOMIC_SET) = reg::uart::IMSC_TXIM;
    }
    void rp_tx_irq_disable(void)
    {
        r32(reg::uart::IMSC + mmap::ATOMIC_CLR) = reg::uart::IMSC_TXIM;
    }

    constexpr uint32_t CONSOLE_TX_SIZE = 512; // power of two; > kprintf's 256B buffer
    char console_tx_buf[CONSOLE_TX_SIZE];
    console_tx_backend const rp_console_backend = {
        rp_tx_slot_free, rp_tx_push, rp_tx_irq_enable, rp_tx_irq_disable};

}

extern "C"
{

void arch_init(void)
{
    // Reset-release ordering is load-bearing (the RP2040 lesson): a peripheral's
    // RESET_DONE only asserts once it has a running clock. IO_BANK0/PADS_BANK0/TIMER0
    // are clocked by clk_sys/clk_ref (already live off the ROSC at reset), so release
    // them now. UART1 is clocked by clk_peri, which is OFF until clocks_init -- release
    // it BEFORE that and its RESET_DONE never asserts, hanging the boot.
    unreset(reg::resets::IO_BANK0 | reg::resets::PADS_BANK0 | reg::resets::TIMER0);
    clocks_init();
    unreset(reg::resets::UART1);
    uart1_init();
#if KICKOS_HAVE_MPU
    kickos_arm_pmsav8_init(); // MAIR + MemManage; first switch enables the MPU
#endif
    kickos_armv7m_init();
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
    *irq_line = irq::UART1_IRQ;
    return &rp_console_backend;
}

// Monotonic clock from the 64-bit system TIMER0 (microseconds -> ns). Uses the
// non-latching RAW halves with a hi/lo/hi re-read to tolerate a 32-bit rollover
// between the reads (core-safe, unlike the latching TIMELR/TIMEHR pair). Overrides
// the arch's weak DWT default: TIMER0 is a true 64-bit source (no 32-bit wrap).
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

// Telemetry trace clock: the low 32 bits of the free-running 1 MHz system TIMER0
// (us, wraps ~71 min). Same source as arch_clock_now (a single RAW-low read), so the
// SESSION-anchor rate is exactly 1000 ns/tick.
uint32_t arch_trace_now(void)
{
    return r32(reg::timer::TIMERAWL);
}

void arch_shutdown(int status)
{
    (void)status; // no exit on bare metal
    __asm volatile("cpsid i" ::: "memory");
    while (true)
    {
        __asm volatile("wfi");
    }
}

#if KICKOS_HAVE_MPU
// Rule 7 reserved set (RP2350 datasheet). Owns-for-life: the 64-bit TIMER0 (monotonic
// base), the TICKS block (its TIMER0 generator is the 1 MHz source -- the RP2040
// watchdog role moved here), and the RESETS + CLOCKS control blocks. Full 16 KB
// windows each so the SET/CLR/XOR atomic aliases are covered. M33 (Arm) has no
// bit-band -> weak arch_bitband_present 0.
size_t arch_reserved_blocks(struct arch_reserved_block* out, size_t max)
{
    static struct arch_reserved_block const blocks[] = {
        {mmap::TIMER0_BASE, mmap::APB_ATOMIC_WINDOW}, // TIMER0: 64-bit us monotonic (DS 12.8)
        {mmap::TICKS_BASE, mmap::APB_ATOMIC_WINDOW},  // TICKS: TIMER0 tick generator, 1 MHz source (DS 8.5)
        {mmap::RESETS_BASE, mmap::APB_ATOMIC_WINDOW}, // RESETS: peripheral reset control (DS 7.5)
        {mmap::CLOCKS_BASE, mmap::APB_ATOMIC_WINDOW}, // CLOCKS: clock generators (DS 8.1)
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
    // The bootrom sets Secure VTOR before entry (datasheet 5.2.2), but pin it
    // explicitly to the image base for robustness (a warm reboot / debugger entry
    // may not have re-run the bootrom path). SCB->VTOR = 0xE000ED08.
    r32(0xE000ED08) = 0x10000000u;

    // Enable the FPU (CP10/CP11 full access) before any code a hard-float ABI might
    // emit FP into -- Cortex-M33 has an FPv5-SP FPU. SCB->CPACR = 0xE000ED88.
    r32(0xE000ED88) |= (0xFu << 20);
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");

    kickos_ranges_init(); // init .data; zero .bss
    for (void (**fn)() = __init_array_start; fn != __init_array_end; fn++)
    {
        (*fn)();
    }
    arch_init();
    kickos::kmain(0, nullptr);
    arch_shutdown(0);
}
}
