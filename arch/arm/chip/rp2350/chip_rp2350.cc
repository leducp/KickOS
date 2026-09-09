// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Raspberry Pi RP2350 (Cortex-M33) chip backend. Register addresses/fields are
// clean-room from the RP2350 datasheet (RP-008373-DS-2); hand-rolled, no vendor
// SDK sources, consistent with the arch layer's regs.h. Section numbers in the
// comments cite that datasheet.
//
// The clock, PLL and console-transport sequences this part shares with the RP2040 are in
// arch/arm/chip/rp2xxx/chip_rp2xxx.cc, which family.cmake adds to this chip's archive.
//
// clk_sys
// is raised to 150 MHz off PLL_SYS (12 MHz XOSC x125 /5 /2, the datasheet default
// max, 8.6); SystemCoreClock tracks it so the SysTick ns<->cycle math
// (arch_arm_common) stays coherent. clk_ref is the XOSC divided by CLK_REF_DIV (8.1),
// which the bootrom leaves at something other than the reset 1, so clk_ref is NOT the
// crystal frequency and the divisor is read at boot. The TICKS TIMER0 generator divides
// clk_ref again to the 1 MHz the 64-bit system TIMER0 (arch_clock_now / arch_trace_now)
// is read at, PLL-independent. clk_peri follows clk_sys, so the UART baud divisors are
// recomputed for 150 MHz. If the crystal or the PLL never comes up the board degrades to
// ROSC timing instead of hanging.
//
// Key deltas from the RP2040 (all APB peripheral bases relocated; datasheet 2.2.4):
//   - No boot2/CRC stage: the bootrom does XIP setup + reads SP/PC from the vector
//     table (startup.S / rp2350.ld).
//   - The system TIMER tick comes from the new common TICKS block (8.5), not the
//     watchdog.
//   - PADS gained an ISO (isolation) bit that resets SET and must be cleared to use
//     a pad (9.11.3).
//   - 52 NVIC lines; the console is on UART1 (UART1_IRQ = 34, 3.2). See the IO_BANK0
//     block below for why the Pi-Zero header forces UART1, not UART0.

#include <kickos/arch/amp_shared.h> // arch_amp_shared_zero: the partition primary's own clear
#include <kickos/arch/arch.h>
#include <kickos/config/limits.h>
#include <kickos/diag.h>
#include <kickos/console_tx.h>
#include <kickos/sys/abi.h> // KOS_E* taxonomy (arch_pinmux_set)
#include <kickos/sys/atomic.h>

#include <stdint.h>

#include <fatal_status.ld.h>

#include <kickos/chip_limits.h> // KICKOS_RP2350_SIO_IRQ_BELL: the doorbell's own NVIC line
#include <kickos/chip_mmap.h>
#include "irq.h"
#include "regs/clocks.h"
#include "regs/io_bank0.h"
#include "regs/pads.h"
#include "regs/pll.h"
#include "regs/resets.h"
#include "regs/sio.h"
#include "regs/ticks.h"
#include "regs/timer.h"
#include "regs/uart.h"
#include "regs/xosc.h"
#include "../rp2xxx/rp2xxx.h"

namespace mmap = kickos::rp2350::mmap;
namespace reg = kickos::rp2350::reg;
namespace irq = kickos::rp2350::irq;

using kickos::rp2xxx::pll_sys_lock;
using kickos::rp2xxx::POLL_TIMEOUT;
using kickos::rp2xxx::r32;
using kickos::rp2xxx::unreset;
using kickos::rp2xxx::wait_mask;

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
// kickos_arch_mpu_commit / arch_mpu_region_encodable replace the v7-M fallback TUs.
void kickos_arm_pmsav8_init(void);
#endif

extern void (*__init_array_start[])();
extern void (*__init_array_end[])();
extern uint32_t g_isr_vector[]; // startup.S: the vector table at this image's flash base

// Pre-init value: clk_sys as the bootrom leaves it. clocks_init() overwrites this on
// every path; SysTick (processor clock) reads it live.
uint32_t SystemCoreClock = reg::clocks::ROSC_NOMINAL_HZ;
}

namespace
{
#if defined(KICKOS_ENABLE_SELFTEST) || KICKOS_AMP_OWN_IMAGE
    // Bootrom header accessors: its magic is bytes and its pointers are halfwords, so neither
    // is reachable through r32.
    // GCC assumes the first min-pagesize bytes are unmapped; 0x0 is the bootrom.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
    inline uint8_t r8(uintptr_t a) { return *reinterpret_cast<volatile uint8_t*>(a); }
    inline uint16_t r16(uintptr_t a) { return *reinterpret_cast<volatile uint16_t*>(a); }
#pragma GCC diagnostic pop
#endif

#if KICKOS_AMP_OWN_IMAGE
    // The clk_sys the primary resolved, published for every other node to install: a peer runs
    // no clocks_init and would otherwise keep the bootrom's reset value while executing off a
    // PLL three times faster. The dsb in arch_amp_release_peers orders this store ahead of any
    // peer's boot, so the field itself carries no ordering.
    KICKOS_AMP_SHARED("chip")
    kickos::Atomic<uint32_t, kickos::Order::RELAXED> g_amp_clk_sys_hz = 0;
#endif

    // Chosen by clocks_init (which source clk_peri lands on), consumed by uart1_init.
    // Boot is single-threaded and sequential, so no guard is needed.
    uint32_t g_uart_ibrd = reg::uart::IBRD_115200;
    uint32_t g_uart_fbrd = reg::uart::FBRD_115200;

#if defined(KICKOS_USB_CONSOLE)
    // PLL_USB at 48 MHz, clk_usb onto it, and the USB block out of reset. All three
    // touch RESETS/CLOCKS, which the MPU reserves for the kernel
    // (arch_reserved_blocks), so the unprivileged driver cannot do them; everything
    // inside the USB block itself is left to it.
    //
    // USB bring-up is refused, at bring-up time, when the crystal did not come up. A
    // full-speed device cannot be sourced from the ring oscillator, and a 6.5 MHz
    // clk_sys also violates the clk_sys > 1.1 * clk_usb workaround for RP2350-E12. The
    // refusal is silent here because the console is not up yet; the driver reports it
    // (its DPRAM reads back as bus errors with the block still in reset).
    void usb_clock_init()
    {
        if (SystemCoreClock < reg::clocks::CLK_SYS_MIN_FOR_USB_HZ)
        {
            return;
        }
        r32(reg::resets::RESET + mmap::ATOMIC_SET) = reg::resets::PLL_USB;
        r32(reg::resets::RESET + mmap::ATOMIC_CLR) = reg::resets::PLL_USB;
        wait_mask(reg::resets::RESET_DONE, reg::resets::PLL_USB);

        r32(reg::pll_usb::CS) = reg::pll::CS_REFDIV_1;
        r32(reg::pll_usb::FBDIV_INT) = reg::pll_usb::FBDIV_100;
        r32(reg::pll_usb::PWR + mmap::ATOMIC_CLR) = reg::pll::PWR_PD | reg::pll::PWR_VCOPD;
        if (not wait_mask(reg::pll_usb::CS, reg::pll::CS_LOCK))
        {
            return; // the block stays in reset: an un-clocked controller never enumerates
        }
        r32(reg::pll_usb::PRIM) = reg::pll_usb::PRIM_POSTDIV;
        r32(reg::pll_usb::PWR + mmap::ATOMIC_CLR) = reg::pll::PWR_POSTDIVPD;

        // clk_usb has no glitchless mux, so the source may only be changed with the
        // generator stopped (datasheet 8.1.3.2). Both writes are absolute values, never
        // read-modify-writes, so no reset-value assumption leaks in.
        r32(reg::clocks::CLK_USB_CTRL) = reg::clocks::CLK_USB_AUXSRC_PLL_USB;
        for (uint32_t i = 0; i < POLL_TIMEOUT; i++)
        {
            if ((r32(reg::clocks::CLK_USB_CTRL) & reg::clocks::CLK_USB_ENABLED) == 0u)
            {
                break;
            }
        }
        r32(reg::clocks::CLK_USB_CTRL) =
            reg::clocks::CLK_USB_AUXSRC_PLL_USB | reg::clocks::CLK_USB_ENABLE;

        // The DPRAM is unreachable until the block is out of reset (datasheet 12.7.1.1.3),
        // and RESET_DONE only asserts once clk_usb runs, hence the ordering above.
        unreset(reg::resets::USBCTRL);
    }
#endif

    // clk_ref is the selected source AFTER CLK_REF_DIV (datasheet 8.1). The register
    // resets to INT=1 but the bootrom overwrites it, so the divisor is read, never assumed.
    uint32_t clk_ref_hz(uint32_t src_hz)
    {
        uint32_t div = (r32(reg::clocks::CLK_REF_DIV) & reg::clocks::CLK_REF_DIV_INT_MASK) >>
                       reg::clocks::CLK_REF_DIV_INT_SHIFT;
        if (div == 0u)
        {
            div = reg::clocks::CLK_REF_DIV_INT_ZERO;
        }
        return src_hz / div;
    }

    // Start the TICKS TIMER0 generator so the 64-bit system TIMER0 counts. arch_clock_now
    // reads that counter as microseconds, so CYCLES has to land the tick on TICK_HZ for the
    // given clk_ref; a wrong divisor scales every sleep, timeout and timestamp on the board.
    // The generator must be stopped before CYCLES is changed (datasheet 8.5.1).
    void ticks_timer0_start(uint32_t ref_hz)
    {
        uint32_t cycles = (ref_hz + (reg::ticks::TICK_HZ / 2u)) / reg::ticks::TICK_HZ;
        if (cycles == 0u)
        {
            cycles = 1u;
        }
        if (cycles > reg::ticks::CYCLES_MAX)
        {
            cycles = reg::ticks::CYCLES_MAX;
        }
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
            // clk_ref <- XOSC (glitchless mux). clk_sys does NOT follow: the bootrom
            // leaves it on the ROSC through its aux mux, so only the switch below moves it.
            // Poll the one-hot SELECTED before proceeding.
            r32(reg::clocks::CLK_REF_CTRL) = reg::clocks::CLK_REF_SRC_XOSC;
            xosc_ok = wait_mask(reg::clocks::CLK_REF_SELECTED, reg::clocks::CLK_REF_SELECTED_XOSC);
        }

        if (not xosc_ok)
        {
            SystemCoreClock = reg::clocks::ROSC_NOMINAL_HZ;            // clk_sys stayed on ROSC
            r32(reg::clocks::CLK_PERI_CTRL) = reg::clocks::CLK_PERI_ENABLE_CLK_SYS; // UART clock <- clk_sys
            ticks_timer0_start(reg::clocks::CLK_REF_ROSC_NOMINAL_HZ);
            return;
        }

        // clk_ref stays on the XOSC: the TICKS TIMER0 tick and thus the 1 MHz system
        // TIMER0 (arch_clock_now / arch_trace_now) derive from clk_ref and MUST NOT track
        // the PLL.
        ticks_timer0_start(clk_ref_hz(reg::xosc::FREQ_HZ));

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
            g_uart_ibrd = reg::uart::IBRD_PLL;
            g_uart_fbrd = reg::uart::FBRD_PLL;
            r32(reg::clocks::CLK_PERI_CTRL) = reg::clocks::CLK_PERI_ENABLE_CLK_SYS; // UART clock <- clk_sys 150 MHz
        }
        else
        {
            // PLL never locked, so clk_sys is untouched and stays on the ROSC the bootrom
            // left it on. clk_peri is taken off the crystal instead, which is what the
            // 12 MHz UART divisor defaults are for.
            SystemCoreClock = reg::clocks::ROSC_NOMINAL_HZ;
            r32(reg::clocks::CLK_PERI_CTRL) = reg::clocks::CLK_PERI_ENABLE_XOSC; // UART clock <- XOSC 12 MHz
        }
    }

    void uart1_init()
    {
        // Route GP4/GP5 to UART1 and make the pads usable. The RP2350 pads reset
        // ISOLATED (PAD_ISO set): clear it or the pad stays disconnected.
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
}

#if KICKOS_AMP_OWN_IMAGE
namespace
{
    // One UART and two kernels, so a chunk is claimed against the peer before its bytes are
    // pushed. The claim is held to the LINE, not to the chunk: console_write_user copies a user
    // buffer in 64-byte pieces and calls the writer once per piece, so a claim released at the
    // end of a chunk would let the peer's whole line land inside one of this node's.
    //
    // Ownership is bounded by a DEADLINE taken when the claim is, never by what the holder
    // writes next: a node that stops mid-line, or dies there, would otherwise keep the lock for
    // as long as it lives and leave every later line unserialised. The holder drops the claim at
    // its first release past the deadline, and a peer that spins its budget out against an
    // expired one releases the register under the holder and takes it. So the bound holds with
    // the holder gone, which is what the console's "always works, even while dying" guarantee
    // needs.
    //
    // A stolen-from node must not then end the thief's claim, so the release is conditional on
    // the published owner still naming this node. The test and the release are not one atomic
    // act: a steal landing between them frees a claim one line early and the thief's own release
    // is then refused by that same test rather than ending a third claim, so the window costs a
    // shredded line and does not cascade.
    //
    // A caller that loses the claim writes anyway; the cost is a shredded line.
    //
    // Interrupts are NOT masked across it: console.cc requires that the chip transport never be
    // held under IrqLock across a transmission.

    // The wire time of the 512-byte run the claim is meant to cover: 512 bytes at 115200 8N1
    // is 44.4 ms. A holder past this is stalled or dead, not writing a long line.
    constexpr uint32_t CONSOLE_HOLD_MAX_US = 50000u;

    // Read by a peer whose own claim failed, so they live where both nodes look. Zeroed by the
    // partition primary before any peer runs (arch_amp_shared_zero). SPINLOCK31 serialises the
    // writers and the dsb pair below carries the ordering, so the fields carry none.
    KICKOS_AMP_SHARED("chip")
    kickos::Atomic<uint32_t, kickos::Order::RELAXED> g_console_owner = 0; // 0 free, else node id + 1
    KICKOS_AMP_SHARED("chip")
    kickos::Atomic<uint32_t, kickos::Order::RELAXED> g_console_deadline = 0; // TIMER0 us

    constexpr uint32_t CONSOLE_OWNER_SELF = KICKOS_AMP_NODE_ID + 1u;

    bool g_console_held = false;

    bool console_hold_expired()
    {
        // Half-range difference: TIMER0's low half wraps every ~71 min and a hold is tens of ms.
        return (r32(reg::timer::TIMERAWL) - g_console_deadline) < 0x80000000u;
    }

    void claim_taken()
    {
        g_console_held = true;
        g_console_deadline = r32(reg::timer::TIMERAWL) + CONSOLE_HOLD_MAX_US;
        __asm volatile("dsb" ::: "memory"); // a peer reading this owner must see its deadline
        g_console_owner = CONSOLE_OWNER_SELF;
    }

}

namespace kickos::rp2xxx
{
    void console_claim(void)
    {
        // A fault landing inside a run this node already holds writes into it rather than
        // spinning its budget out against its own claim. One flag serves: this node drives one
        // core.
        if (g_console_held)
        {
            return;
        }
        // The wait and the grant it waits out are both TIMER0 microseconds. Counted in loop
        // iterations instead, the wait grows with a degraded core clock while the grant does
        // not, and a contender whose patience has outrun the grant takes a line its holder is
        // still legitimately inside. One grant's length from here covers any claim taken
        // before this call.
        uint32_t const waited_from = r32(reg::timer::TIMERAWL);
        // KICKOS_POLL_SPIN_MAX is the structural backstop and not the budget: a TIMER0 that has
        // stopped must still leave this node able to emit.
        for (uint32_t i = 0; i < KICKOS_POLL_SPIN_MAX; i++)
        {
            if (r32(reg::sio::SPINLOCK31) != 0)
            {
                claim_taken();
                return;
            }
            if ((r32(reg::timer::TIMERAWL) - waited_from) >= CONSOLE_HOLD_MAX_US)
            {
                break;
            }
        }
        // A claim past its bound, so its holder is stalled mid-line or gone. Any write to the
        // register releases it, whichever core claimed it.
        if (g_console_owner != 0 and console_hold_expired())
        {
            r32(reg::sio::SPINLOCK31) = 1u;
            if (r32(reg::sio::SPINLOCK31) != 0)
            {
                claim_taken();
            }
        }
    }

    void console_drop(bool ended_line)
    {
        if (not g_console_held)
        {
            return;
        }
        if (not ended_line and not console_hold_expired())
        {
            return;
        }
        g_console_held = false;
        if (g_console_owner != CONSOLE_OWNER_SELF)
        {
            return;
        }
        g_console_owner = 0;
        __asm volatile("dsb" ::: "memory"); // no peer may read this node as owner past the free
        r32(reg::sio::SPINLOCK31) = 1u;
    }
}
#endif

#if !KICKOS_AMP_OWN_IMAGE
namespace
{
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
#endif

extern "C"
{

void arch_init(void)
{
    // Reset-release ordering is load-bearing: a peripheral's RESET_DONE only asserts
    // once it has a running clock. IO_BANK0/PADS_BANK0/TIMER0 are clocked by
    // clk_sys/clk_ref (already live off the ROSC at reset), so release them now. UART1
    // is clocked by clk_peri, which is OFF until clocks_init: release it BEFORE that
    // and its RESET_DONE never asserts, hanging the boot.
    // The clock tree, the peripheral resets and the console UART are partition-wide, so only
    // the PRIMARY brings them up. A peer re-entering clocks_init stops clk_sys under a primary
    // executing off those PLLs, which the datasheet documents as an unrecoverable lock-up, and
    // drives UART1 through a reset mid-transmission; both present as the primary wedging.
    //
    // A peer still owes its own SystemCoreClock, every delay, timeout and baud divisor being
    // derived from it. It is installed from the primary's resolved answer, never re-derived.
#if KICKOS_AMP_OWN_IMAGE
    if (KICKOS_AMP_NODE_ID != 0)
    {
        uint32_t const hz = g_amp_clk_sys_hz;
        if (hz != 0)
        {
            SystemCoreClock = hz;
        }
    }
    else
#endif
    {
        unreset(reg::resets::IO_BANK0 | reg::resets::PADS_BANK0 | reg::resets::TIMER0);
        clocks_init();
        unreset(reg::resets::UART1);
        uart1_init();
#if defined(KICKOS_USB_CONSOLE)
        usb_clock_init(); // after clocks_init: PLL_USB needs the crystal verdict
#endif
#if KICKOS_AMP_OWN_IMAGE
        g_amp_clk_sys_hz = SystemCoreClock;
        // Before any peer exists, so the release cannot land on a claim in progress.
        r32(reg::sio::SPINLOCK31) = 1u;
#endif
    }
#if KICKOS_HAVE_MPU
    kickos_arm_pmsav8_init(); // MAIR + MemManage; first switch enables the MPU
#endif
    kickos_armv7m_init();
#if (KICKOS_NUM_CORES > 1 || KICKOS_AMP_NODE)
    // SIO_IRQ_BELL is CORE-LOCAL (datasheet 3.1.6), so every node opens its own. Through
    // arch_irq_unmask for the priority: the NVIC IPR resets to 0, and a doorbell above the
    // BASEPRI band runs its service inside a section holding IrqLock.
    arch_irq_unmask(KICKOS_RP2350_SIO_IRQ_BELL);
#endif
}

bool arch_irq_line_kernel_owned(int line)
{
#if (KICKOS_NUM_CORES > 1 || KICKOS_AMP_NODE)
    return line == KICKOS_RP2350_SIO_IRQ_BELL;
#else
    (void)line;
    return false;
#endif
}

console_tx_backend const* arch_console_tx_backend(char** storage, uint32_t* size, int* irq_line)
{
#if KICKOS_AMP_OWN_IMAGE
    // No buffered path on a partition: the console contract's "TX IRQ enabled whenever the ring
    // is non-empty" is stated of ONE ring, and IMSC.TXIM is one bit over two. UART1_IRQ reaches
    // both cores' controllers (datasheet 3.2), so whichever node's drain reaches empty first
    // clears the bit the other's queued bytes are waiting on. With no ring nothing writes IMSC,
    // TXIM stays 0, and the polled writer above is this node's one writer.
    (void)storage;
    (void)size;
    (void)irq_line;
    return nullptr;
#else
    *storage = console_tx_buf;
    *size = CONSOLE_TX_SIZE;
    *irq_line = irq::UART1_IRQ;
    return &rp_console_backend;
#endif
}

// Kernel-owned pins arch_pinmux_set refuses so a board map cannot dark the console.
// GP4/GP5 = UART1 TX/RX (the only console pins on the Pi-Zero header). No diag LED.
static bool rp2350_pin_kernel_owned(uint32_t pin)
{
    return pin == 4u or pin == 5u;
}

// One-shot pin-function config (KOS_SYS_PINMUX_SET). func packs the IO_BANK0 CTRL
// funcsel in bits[4:0] plus pad/SIO side effects: bit[8] set pad IE, bit[9] clear
// pad OD (drive out), bit[16] enable the SIO output (GPIO_OE_SET, 1<<pin). The pad
// ISO bit is ALWAYS cleared: RP2350 pads reset ISOLATED and stay dead otherwise. IE
// resets 0 here, so a peripheral INPUT requires bit[8]. IO_BANK0/PADS are already
// unreset+clocked from arch_init, so no clock gate is needed.
int arch_pinmux_set(uint32_t port, uint32_t pin, uint32_t func)
{
    if (port != 0u or pin > 29u)
    {
        return -KOS_EINVAL;
    }
    if (rp2350_pin_kernel_owned(pin))
    {
        return -KOS_EBUSY;
    }
    r32(reg::io_bank0::gpio_ctrl(pin)) = func & 0x1fu;
    uint32_t clr = reg::pads::ISO;
    if ((func & (1u << 9)) != 0u)
    {
        clr |= reg::pads::OD;
    }
    r32(reg::pads::gpio(pin) + mmap::ATOMIC_CLR) = clr;
    if ((func & (1u << 8)) != 0u)
    {
        r32(reg::pads::gpio(pin) + mmap::ATOMIC_SET) = reg::pads::IE;
    }
    if ((func & (1u << 16)) != 0u)
    {
        r32(reg::sio::GPIO_OE_SET) = 1u << pin;
    }
    return 0;
}

#if defined(KICKOS_ENABLE_SELFTEST)
// Reboot into BOOTSEL (firmware-download) mode via the bootrom `reboot` entry. The
// header differs from the RP2040's: the magic third byte is 0x02, and the Arm lookup
// helper (rom_table_lookup_val, no table argument) is the halfword at 0x16; 0x18 is a
// different entry here. Once the magic matches, those pointers are valid. The byte at
// 0x13 is a ROM build number the datasheet forbids using to locate functions.
int arch_reboot(void)
{
    __asm volatile("cpsid i" ::: "memory"); // dispatch runs in thread mode with IRQs live

    if (r8(0x10u) != 'M' or r8(0x11u) != 'u' or r8(0x12u) != 0x02u)
    {
        return -KOS_ENOSYS;
    }
    // Zero-extend the stored halfword: it already carries the Thumb bit.
    using lookup_fn = void* (*)(uint32_t, uint32_t);
    lookup_fn const lookup =
        reinterpret_cast<lookup_fn>(static_cast<uintptr_t>(r16(0x16u)));

    // Function code 'RB' packs as (c2 << 8) | c1; RT_FLAG_FUNC_ARM_SEC picks the
    // Secure Arm variant (KickOS runs M33 Secure, its IMAGE_DEF marks Security=S).
    constexpr uint32_t RT_FLAG_FUNC_ARM_SEC = 0x0004u;
    using reboot_fn = int (*)(uint32_t, uint32_t, uint32_t, uint32_t);
    reboot_fn const rom_reboot = reinterpret_cast<reboot_fn>(
        lookup(('B' << 8) | 'R', RT_FLAG_FUNC_ARM_SEC));
    if (rom_reboot == nullptr)
    {
        return -KOS_ENOSYS; // magic matched, but this ROM's table has no 'RB'
    }

    constexpr uint32_t REBOOT2_FLAG_REBOOT_TYPE_BOOTSEL = 0x0002u;
    constexpr uint32_t REBOOT2_FLAG_NO_RETURN_ON_SUCCESS = 0x0100u;
    char const REBOOT_RETURNED_NL[] = "\n";
    // delay_ms must never be 0: the bootrom mishandles a zero timeout, which pico-sdk
    // works around by forcing 1. Not in the datasheet errata.
    rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_BOOTSEL | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS,
               10u, 0u, 0u);
    // NOT kpanic: this is reached from kfault_terminate, where kpanic itself ends, so
    // panicking here re-enters a console kpanic_enter has already reclaimed. The polled writer
    // is the one arch.h states is safe with interrupts down.
    arch_console_write_sync(kickos::diag::kRebootRp2350,
                            sizeof(kickos::diag::kRebootRp2350) - 1);
    arch_console_write_sync(REBOOT_RETURNED_NL, sizeof(REBOOT_RETURNED_NL) - 1);
    arch_shutdown(KICKOS_FATAL_STATUS);
}
#endif

#if KICKOS_HAVE_MPU
// Rule 7 reserved set (RP2350 datasheet). Owns-for-life: the 64-bit TIMER0 (monotonic
// base), the TICKS block (its TIMER0 generator is the 1 MHz source), and the
// RESETS + CLOCKS control blocks. Full 16 KB
// windows each so the SET/CLR/XOR atomic aliases are covered. M33 (Arm) has no
// bit-band -> the arch_bitband_present fallback 0 stands.
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

#if KICKOS_AMP_OWN_IMAGE
// Restores the reset QMI address translation, an identity map over the whole 16 MiB (datasheet
// Tables 1302-1305, pp.1247-1249; prose at 12.14.4, p.1234). Node 0 reads its peer's vector
// table out of flash to launch it, and that address is the peer's only under an identity map.
// The bootrom reprograms ATRANS when the booted image sits in a flash partition (12.14.4.1,
// p.1235), and a ROLLING_WINDOW_DELTA item in node 0's IMAGE_DEF makes it do so too (5.9.3.5,
// pp.424-425); this partition uses neither.
//
// BOTH CALLS MUST RUN BEFORE CORE 1 IS LAUNCHED. The flush "unpins pinned cache lines"
// (5.4.8.8, p.386) across the ONE cache both nodes share, so the same call once a peer is
// running would take that peer's cache-as-SRAM. Nothing is pinned yet: the bootrom invalidates
// every line on entering flash boot (4.4.1.2, p.343).
void kickos_rp2350_xip_identity(void)
{
    if (KICKOS_AMP_NODE_ID != 0)
    {
        return;
    }
    if (r8(0x10u) != 'M' or r8(0x11u) != 'u' or r8(0x12u) != 0x02u)
    {
        return; // no bootrom table here; the reset state is what it already is
    }
    using lookup_fn = void* (*)(uint32_t, uint32_t);
    lookup_fn const lookup =
        reinterpret_cast<lookup_fn>(static_cast<uintptr_t>(r16(0x16u)));
    constexpr uint32_t RT_FLAG_FUNC_ARM_SEC = 0x0004u;
    using void_fn = void (*)(void);

    // rom_table_code(c1, c2) is (c2 << 8) | c1 (5.4.1, p.378). WHICH LISTED CHARACTER IS c1 is
    // not stated anywhere in the datasheet, so the byte order here matches the 'R','B' spelling
    // arch_reboot already uses.
    void_fn const reset_trans =
        reinterpret_cast<void_fn>(lookup(('A' << 8) | 'R', RT_FLAG_FUNC_ARM_SEC));
    void_fn const flush_cache =
        reinterpret_cast<void_fn>(lookup(('C' << 8) | 'F', RT_FLAG_FUNC_ARM_SEC));
    if (reset_trans != nullptr)
    {
        reset_trans();
    }
    // Required after a translation change (Tables 1302-1305), and harmless when the call above
    // changed nothing.
    if (flush_cache != nullptr)
    {
        flush_cache();
    }
}
#endif

void Reset_Handler(void)
{
    // The bootrom sets Secure VTOR before entry (datasheet 5.2.2), but pin it
    // explicitly to the image base for robustness (a warm reboot / debugger entry
    // may not have re-run the bootrom path). SCB->VTOR = 0xE000ED08.
    // From the SYMBOL and never a literal: a peer links at its own flash slice, and a
    // literal base would point its table at node 0's handlers.
    r32(0xE000ED08) = reinterpret_cast<uintptr_t>(g_isr_vector);

    // Enable the FPU (CP10/CP11 full access) before any code a hard-float ABI might
    // emit FP into; Cortex-M33 has an FPv5-SP FPU. SCB->CPACR = 0xE000ED88.
    r32(0xE000ED88) |= (0xFu << 20);
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");

    kickos_ranges_init(); // init .data; zero .bss
#if KICKOS_AMP_OWN_IMAGE
    kickos_rp2350_xip_identity();
    // Ahead of arch_init, which publishes into that region; later would erase the primary's
    // own publication. The region is outside .bss, so kickos_ranges_init does not reach it.
    arch_amp_shared_zero();
#endif
    for (void (**fn)() = __init_array_start; fn != __init_array_end; fn++)
    {
        (*fn)();
    }
    arch_init();
    kickos::kmain(0, nullptr);
    arch_shutdown(0);
}
}
