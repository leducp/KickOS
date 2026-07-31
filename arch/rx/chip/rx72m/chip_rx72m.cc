// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RX72M chip backend. Register addresses/fields are from the RX72M Group User's
// Manual: Hardware (r01uh0804ej0120, Rev.1.20); hand-rolled (no vendor SDK),
// consistent with the arch layer's clean-room regs.h.
//
// Board: RX72M CPU Card with RDC-IC (RTK0EMXDE0C00000BJ), R5F572MNDDBD, 24 MHz
// main crystal (board UM r12uz0098ej0110 Table 1-1). Console = SCI6 on PB1/TXD6
// + PB0/RXD6 (board Table 5-4, CN6/CN7 "Renesas Motor Workbench" serial). Diag
// LED = LED6 on P80, active-low (board Table 5-9).
//
// Clock target: ICLK 240 MHz from the 24 MHz crystal via PLL (the part's max --
// UM sec.9 / datasheet fPLL 120-240, ICLK max 240). PLL VCO = 24 MHz /1 x10 = 240;
// ICLK = /1. Above 120 MHz the code flash needs one wait state, so MEMWAIT is set
// to 1 (and read back) before the PLL runs, per UM sec.9.8 case (1). Peripheral clocks
// stay inside their ceilings: PCLKA = 120 (/2, max 120), PCLKB/C/D = FCLK = BCLK =
// 60 (/4). PCLKB = 60 MHz is the SCI + CMTW clock -- unchanged from the 120 MHz
// bring-up, so the console baud and timer tick math are identical.

#include <kickos/arch/arch.h>
#include <kickos/console_tx.h>
#include <kickos/sys/abi.h> // KOS_E* taxonomy (arch_pinmux_set)

#include <stdint.h>

// Bases in mmap.h, IRQ vectors in irq.h, per-peripheral offsets/fields in regs/.
#include "mmap.h"
#include "irq.h"
#include "regs/cgc.h"
#include "regs/flash.h"
#include "regs/icu.h"
#include "regs/mpc.h"
#include "regs/port.h"
#include "regs/sci.h"

namespace mmap = kickos::rx::mmap;
namespace irq = kickos::rx::irq;
namespace cgc = kickos::rx::reg::cgc;
namespace flash = kickos::rx::reg::flash;
namespace icu = kickos::rx::reg::icu;
namespace mpc = kickos::rx::reg::mpc;
namespace port = kickos::rx::reg::port;
namespace sci = kickos::rx::reg::sci;

namespace kickos
{
    int kmain(int argc, char** argv);
}

extern "C"
{
    void kickos_rxv3_init(void);

    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();

    // CMTW input clock (PCLKB / 8) the arch clock+timer convert against. Set to
    // the achieved value once the PLL is confirmed locked (arch_init); left at
    // the LOCO reset nominal if the bring-up degrades so timing stays plausible.
    uint32_t kickos_rx_timer_hz = 30000u; // ~LOCO/8 until PLL locks

    // ICLK core clock in Hz (CMSIS-style). LOCO reset nominal until the PLL
    // bring-up in arch_init raises it to the achieved 240 MHz.
    uint32_t SystemCoreClock = 240000u; // LOCO reset nominal (raised to 240 MHz on PLL lock)
}

namespace
{
    inline volatile uint32_t& r32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }
    inline volatile uint16_t& r16(uintptr_t a) { return *reinterpret_cast<volatile uint16_t*>(a); }
    inline volatile uint8_t& r8(uintptr_t a) { return *reinterpret_cast<volatile uint8_t*>(a); }

    // Bounded-poll ceilings: a clock/console misconfiguration must degrade (fall
    // through), never hang the boot. Sized generously vs. the LOCO-clocked worst
    // case (osc/PLL settling counts run off the ~240 kHz LOCO).
    constexpr uint32_t CLOCK_POLL_LIMIT = 2000000u;
    constexpr uint32_t CONSOLE_POLL_LIMIT = 1000000u;

    void unlock_registers(bool on)
    {
        if (on)
        {
            r16(cgc::PRCR) = cgc::PRCR_UNLOCK;
        }
        else
        {
            r16(cgc::PRCR) = cgc::PRCR_LOCK;
        }
    }

    // A PmnPFS write lands ONLY inside this bracket (UM sec.23.2.1): B0WI must be
    // cleared before PFSWE is writable, so the unlock is two writes, not one. Without
    // it the PFS write is dropped silently.
    void mpc_pfs_unlock(bool on)
    {
        if (on)
        {
            r8(mpc::PWPR) = 0x00;
            r8(mpc::PWPR) = mpc::PWPR_PFSWE;
        }
        else
        {
            r8(mpc::PWPR) = mpc::PWPR_B0WI;
        }
    }

    bool poll_flag(uintptr_t reg, uint8_t mask, uint32_t limit)
    {
        for (uint32_t i = 0; i < limit; i++)
        {
            if ((r8(reg) & mask) != 0)
            {
                return true;
            }
        }
        return false;
    }

    // Bring ICLK to 240 MHz via the PLL (UM sec.9.6 procedure, case 1: LOCO -> PLL,
    // main clock as the PLL source). Returns false (leaving the chip on the LOCO
    // reset clock) if the oscillator or PLL never reports stable -- a degraded but
    // non-hanging boot. Must run inside the PRCR unlock (clock regs are PRC0).
    bool clock_to_pll_240mhz()
    {
        r8(cgc::MOFCR) = cgc::MOFCR_XTAL_24MHZ;    // 1. crystal drive range
        r8(cgc::MOSCWTCR) = cgc::MOSCWTCR_MSTS;    // 2. oscillation settling count
        r8(cgc::MOSCCR) = cgc::MOSCCR_MAIN_RUN;    // 3. start the main clock oscillator
        uint8_t mosccr_rb = r8(cgc::MOSCCR);       // read back before dependent writes (sec.9.2.8)
        (void)mosccr_rb;
        if (not poll_flag(cgc::OSCOVFSR, cgc::OSCOVFSR_MOOVF, CLOCK_POLL_LIMIT))
        {
            return false;
        }
        // ICLK will exceed 120 MHz, so the code flash needs one wait state; set it
        // BEFORE running the PLL, per the sec.9.8 case (1) step order (step 4, ahead of
        // PLLCR). Read back so it is in effect before ICLK can rise. MEMWAIT=1 is
        // legal at any ICLK (Table 9.3), so it is harmless if the PLL never locks.
        r8(cgc::MEMWAIT) = cgc::MEMWAIT_ONE_WAIT;  // 4. one wait state (>120 MHz)
        uint8_t memwait_rb = r8(cgc::MEMWAIT);
        (void)memwait_rb;
        r16(cgc::PLLCR) = cgc::PLLCR_PLL_240MHZ;   // 5. multiplier + input divider
        r8(cgc::PLLCR2) = cgc::PLLCR2_PLL_RUN;     // 6. run the PLL
        if (not poll_flag(cgc::OSCOVFSR, cgc::OSCOVFSR_PLOVF, CLOCK_POLL_LIMIT))
        {
            return false;
        }
        // Set the dividers BEFORE the source switch: while still on the LOCO the
        // divisors apply to ~240 kHz, so no peripheral overshoots when the PLL
        // becomes the source on the next write.
        r32(cgc::SCKCR) = cgc::SCKCR_240MHZ;
        r16(cgc::SCKCR3) = cgc::SCKCR3_CKSEL_PLL;  // 8. ICLK <- PLL
        return true;
    }

    // Enable the 8 KB flash ROM cache (UM sec.64.7.1). Reset auto-invalidates it, so
    // there is no coherency risk today; a future flash self-program must re-invalidate
    // (write ROMCIV=1) before re-enable. Bounded invalidate poll degrades, never hangs.
    void rom_cache_enable()
    {
        r16(flash::ROMCIV) = flash::ROMCIV_ROMCIV;
        for (uint32_t i = 0; i < CLOCK_POLL_LIMIT; i++)
        {
            if ((r16(flash::ROMCIV) & flash::ROMCIV_ROMCIV) == 0)
            {
                break;
            }
        }
        r16(flash::ROMCE) = flash::ROMCE_ROMCEN;
    }

    void sci6_console_init()
    {
        // Pin mux: route PB1->TXD6, PB0->RXD6 (UM sec.23.4.1 procedure). PSEL is only
        // writable while the pin's PMR bit is 0, which it is at reset. Without the PMR
        // step the pins stay GPIO.
        mpc_pfs_unlock(true);
        r8(mpc::PB1PFS) = mpc::PFS_PSEL_SCI6; // TXD6
        r8(mpc::PB0PFS) = mpc::PFS_PSEL_SCI6; // RXD6
        mpc_pfs_unlock(false);
        r8(port::PORTB_PMR) |= port::PB1 | port::PB0; // PB1,PB0 -> peripheral function

        r8(sci::SCR) = 0;                 // TE/RE off while configuring
        r8(sci::SMR) = 0;                 // async, 8-bit, no parity, 1 stop, CKS=00
        r8(sci::SEMR) = sci::SEMR_115200; // BGDM+ABCS
        r8(sci::BRR) = sci::BRR_115200;
        for (volatile uint32_t d = 0; d < 10000u; d++) // >= 1-bit settle before TE (sec.42 init)
        {
        }
        r8(sci::SCR) = sci::SCR_TE;       // enable transmitter (TIE off; the ring primes it)
    }

    int rx_tx_slot_free(void) { return (r8(sci::SSR) & sci::SSR_TDRE) != 0; }
    void rx_tx_push(uint8_t b) { r8(sci::TDR) = b; }
    void rx_tx_irq_enable(void) { r8(sci::SCR) = static_cast<uint8_t>(r8(sci::SCR) | sci::SCR_TIE); }
    void rx_tx_irq_disable(void) { r8(sci::SCR) = static_cast<uint8_t>(r8(sci::SCR) & ~sci::SCR_TIE); }

    constexpr uint32_t CONSOLE_TX_SIZE = 512; // power of two; > kprintf's 256B buffer
    char console_tx_buf[CONSOLE_TX_SIZE];
    console_tx_backend const rx_console_backend = {
        rx_tx_slot_free, rx_tx_push, rx_tx_irq_enable, rx_tx_irq_disable};
}

extern "C"
{

void arch_init(void)
{
    unlock_registers(true);
    bool on_pll = clock_to_pll_240mhz();
    // Release the module stops for the timer + console (UM sec.11 MSTPCR).
    r32(cgc::MSTPCRA) &= ~(cgc::MSTPA_CMTW0 | cgc::MSTPA_CMTW1);
    r32(cgc::MSTPCRB) &= ~cgc::MSTPB_SCI6;
    unlock_registers(false);

    rom_cache_enable(); // MEMWAIT set above; ROM cache is not PRCR-gated

    if (on_pll)
    {
        SystemCoreClock = cgc::ICLK_HZ;
        kickos_rx_timer_hz = cgc::PCLKB_DIV8_HZ;
    }

    sci6_console_init();

    // Timer line (CMTW0 compare match, vector 30): priority below the kernel lock
    // level, then enable at the ICU. (The CMTW's own CMWIE is set per-arm.)
    r8(icu::IPR006) = 4; // IPL_DEVICE (< IPL_LOCK)
    r8(icu::IER03) |= icu::IER03_CMWI0;

    kickos_rxv3_init(); // start CMTW1 free-run + reset arch software state
}

void arch_console_write(char const* buf, size_t n)
{
    console_tx_write(buf, n); // buffered; the routing guard (console.cc) keeps this thread-only
}

// Bounded polled writer: panic / fault / pre-arm boot route here (console.cc). Must
// stay reachable with the scheduler and IRQs down -- no ring, no interrupt.
void arch_console_write_sync(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        uint32_t spin = 0;
        while ((r8(sci::SSR) & sci::SSR_TDRE) == 0)
        {
            if (++spin >= CONSOLE_POLL_LIMIT)
            {
                return; // TDRE never cleared (SCI dead/misconfigured): drop, don't hang
            }
        }
        r8(sci::TDR) = static_cast<uint8_t>(buf[i]);
    }
}

// Arch seam (console_tx.h): hand the kernel the SCI6 backend + ring storage + the
// TXI6 line. console_buffer_init binds the drain ISR, unmasks the line (IPR087 +
// IER0A.IEN7 via arch_irq_unmask), and arms the ring.
console_tx_backend const* arch_console_tx_backend(char** storage, uint32_t* size, int* irq_line)
{
    *storage = console_tx_buf;
    *size = CONSOLE_TX_SIZE;
    *irq_line = irq::SCI6_TXI;
    return &rx_console_backend;
}

void arch_diag_led_init(void)
{
    r8(port::PORT8_PMR) &= ~port::LED6;   // GPIO (not peripheral)
    r8(port::PORT8_PODR) |= port::LED6;   // drive high => LED off (active-low, board Table 5-9)
    r8(port::PORT8_PDR) |= port::LED6;    // output
}

void arch_diag_led_set(int on)
{
    if (on != 0)
    {
        r8(port::PORT8_PODR) &= ~port::LED6; // low => LED on
    }
    else
    {
        r8(port::PORT8_PODR) |= port::LED6;  // high => LED off
    }
}

// Kernel-owned pins arch_pinmux_set refuses so a board map or an app cannot dark the
// console: PB1/TXD6 + PB0/RXD6, muxed for life by sci6_console_init.
static bool rx72m_pin_kernel_owned(uint32_t p, uint32_t pin)
{
    return p == 0x0Bu and pin <= 1u;
}

// One-shot pin-function config (KOS_SYS_PINMUX_SET), covering BOTH mux stages an RX
// pin passes: the MPC PmnPFS peripheral-function select and the PORTm.PMR general-I/O
// vs peripheral switch. Mediating PMR alone would leave the refusal above bypassable
// -- PSEL can re-point a pin already at PMR=1 (the console pins are) at a different
// module without PMR ever being written. func packs both stages; the encoding is
// chip-local (reg::mpc::PINMUX_*).
int arch_pinmux_set(uint32_t p, uint32_t pin, uint32_t func)
{
    if (p > port::PORT_INDEX_MAX or pin > port::PIN_MAX)
    {
        return -KOS_EINVAL;
    }
    if ((func & mpc::PINMUX_RESERVED) != 0u)
    {
        return -KOS_EINVAL;
    }
    if (rx72m_pin_kernel_owned(p, pin))
    {
        return -KOS_EBUSY;
    }
    uintptr_t const pmr = port::pmr(p);
    uint8_t const bit = static_cast<uint8_t>(1u << pin);
    if ((func & mpc::PINMUX_PFS_EN) != 0u)
    {
        // PSEL may only change while this pin's PMR bit is 0, else the pin emits an
        // unexpected edge (UM sec.23.4.2 (1)). Clearing it first is step 1 of sec.23.4.1.
        r8(pmr) = static_cast<uint8_t>(r8(pmr) & ~bit);
        mpc_pfs_unlock(true);
        r8(mpc::pfs(p, pin)) = static_cast<uint8_t>(func & mpc::PINMUX_PFS_MASK);
        mpc_pfs_unlock(false);
    }
    if ((func & mpc::PINMUX_PMR) != 0u)
    {
        r8(pmr) = static_cast<uint8_t>(r8(pmr) | bit);
    }
    else
    {
        r8(pmr) = static_cast<uint8_t>(r8(pmr) & ~bit);
    }
    return 0;
}

void arch_shutdown(int status)
{
    (void)status; // no exit on bare metal
    __asm volatile("mvtipl #15" ::: "memory"); // mask all maskable interrupts
    while (true)
    {
        __asm volatile("wait");
    }
}

#if KICKOS_HAVE_MPU
// Rule 7 reserved set (RX72M UM). Owns-for-life: the CMTW time base (both units --
// CMTW0 @0x94200 timebase and CMTW1 @0x94280 bench/trace clock fit one 0x100 block),
// the ICU (the RX IRQ controller is MPU-GOVERNED memory, unlike the ARM PPB, so it
// must be reserved -- the kernel uses IER @0x87203 and IPR @0x87306, so the window
// spans IR/IER/IPR = 0x400, NOT the 0x300 that would miss IPR), the bus-side MPU
// register file, and the SYSTEM clock/reset gate block.
size_t arch_reserved_blocks(struct arch_reserved_block* out, size_t max)
{
    static struct arch_reserved_block const blocks[] = {
        {mmap::CMTW0, 0x100u}, // CMTW0 + CMTW1: time base + bench clock (UM sec.28)
        {mmap::ICU, 0x400u},   // ICU: IR + IER + IPR (UM sec.15) -- MPU-governed IRQ controller
        {mmap::MPU, 0x140u},   // MPU: RSPAGE/REPAGE + MPEN/MPBAC/MPOPI register file (UM sec.17)
        {mmap::SYSTEM, 0x100u}, // SYSTEM: MSTPCR / SCKCR / PLLCR clock+reset gates (UM sec.9/11)
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

// C runtime init, reached from _start (startup.S). Never returns.
void rx_reset_handler(void)
{
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
