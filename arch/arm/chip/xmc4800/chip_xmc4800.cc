// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Infineon XMC4800 (XMC4800 Relax Kit, Cortex-M4F) chip backend (XMC4700/XMC4800 RM V1.3).
//
// clock_init() runs the SCU from the 12 MHz crystal PLL: the uncalibrated fOFI (~24 MHz) is too
// inaccurate for a stable UART baud.

#include "regs.h"
#include <kickos/chip_mmap.h>
#include "board_pins.h"
#include "regs/ccu4.h"
#include "regs/flash.h"
#include "regs/port.h"
#include "regs/scu.h"
#include "regs/usic.h"

#include <kickos/arch/arch.h>
#include <kickos/arch/bounded_wait.h>
#include "pin_guard.h"
#include <kickos/arch/clk_anchor.h>
#include <kickos/console_tx.h>
#include <kickos/io/mmio.h>

#include <kickos/sys/abi.h>

#include <stdint.h>

namespace mmap = kickos::xmc::mmap;
namespace scu = kickos::xmc::reg::scu;
namespace ccu4 = kickos::xmc::reg::ccu4;
namespace flash = kickos::xmc::reg::flash;
namespace rp = kickos::xmc::reg::port;

namespace
{
    constexpr uintptr_t USIC_CHANNEL_STRIDE = mmap::USIC0_CH1_BASE - mmap::USIC0_CH0_BASE;
    constexpr uintptr_t USIC_MODULE_SPAN = 2u * USIC_CHANNEL_STRIDE;
}

namespace kickos
{
    int kmain(int argc, char** argv);
}

extern "C"
{
    void kickos_armv7m_init(void);
    void kickos_xmc_usic_init(void);
    bool kickos_xmc_usic_write(char const* buf, size_t n);

    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();

    uint32_t SystemCoreClock = 144000000u; // fCPU after clock_init (drives SysTick)
}

namespace
{
    // ---- Clock tree: 12 MHz XTAL -> system PLL -> fSYS=fCPU=144 MHz -----------
    // fVCO = 12 MHz * NDIV/PDIV = 288 MHz; fPLL = fVCO/K2DIV. Each PLLCON1 field is written as
    // (value-1).
    uint32_t pllcon1_value(uint32_t k2div)
    {
        return ((scu::PLL_NDIV - 1u) << scu::PLLCON1_NDIV_SHIFT)
             | ((k2div - 1u) << scu::PLLCON1_K2DIV_SHIFT)
             | ((scu::PLL_PDIV - 1u) << scu::PLLCON1_PDIV_SHIFT);
    }

    // --- CCU40: the monotonic time base (RM ch.23) ------------------------------
    // Not DWT_CYCCNT: its reads are unreliable on this silicon (observed returning DWT_CTRL's
    // value), and a 32->64 wrap extension turns one bad read into a phantom 2^32 jump that
    // strands every timed wait. CCU40's four 16-bit slices are chained into ONE free-running
    // 64-bit HARDWARE counter on fCCU, so no read can manufacture a wrap.
    //
    // CLKSET.CCUCEN alone is not enough: CCU40 also comes out of SCU reset CLOCK-GATED (CGATCLR0)
    // and held in PERIPHERAL RESET (PRCLR0), and needs the prescaler run bit (GIDLC.SPRB).
    // SLEEPCR.CCUCR keeps fCCU running in SLEEP: the idle path is a plain WFI and the CCU clock
    // gates off in SLEEP by default, which would freeze this counter on every tickless idle. A
    // DEEPSLEEP user must set DSLEEPCR too.

    inline volatile uint32_t& cc4(unsigned slice, uintptr_t reg)
    {
        return r32(ccu4::slice(slice, reg));
    }

    void ccu4_clock_init()
    {
        // arch_clock_now MUST NOT run before this: a TIMER read of the gated CCU40 BusFaults.
        r32(scu::CLKSET) = scu::CLKSET_CCUCEN;
        r32(scu::CCUCLKCR) &= ~scu::CCUCLKCR_CCUDIV;    // fCCU = fSYS
        r32(scu::CGATCLR0) = scu::CCU40_GATE_BIT;
        r32(scu::PRCLR0) = scu::CCU40_RESET_BIT;
        r32(scu::SLEEPCR) |= scu::SLEEPCR_CCUCR;

        // Without SPRB the slice clock never runs and every TIMER stays 0.
        r32(ccu4::GIDLC) = ccu4::GIDLC_SPRB | ccu4::GIDLC_CLEAR_ALL;

        for (unsigned s = 0; s < 4; s++)
        {
            cc4(s, ccu4::slice_off::TC) = 0;
            cc4(s, ccu4::slice_off::PSC) = 0;                  // the linked slices ignore PSC
            cc4(s, ccu4::slice_off::PRS) = ccu4::PERIOD_MAX;   // 0xFFFF carries into the next slice
        }
        // CC41..CC43 count the overflow of the slice below; CC40 (the 16 LSBs) must keep TCE=0
        // (RM 23.2.9).
        cc4(0, ccu4::slice_off::CMC) = 0;
        cc4(1, ccu4::slice_off::CMC) = ccu4::CMC_TCE;
        cc4(2, ccu4::slice_off::CMC) = ccu4::CMC_TCE;
        cc4(3, ccu4::slice_off::CMC) = ccu4::CMC_TCE;

        r32(ccu4::GCSS) = ccu4::GCSS_SHADOW_ALL; // transfer PRS -> PR for every slice

        for (unsigned s = 0; s < 4; s++)
        {
            cc4(s, ccu4::slice_off::TCSET) = ccu4::TCSET_TRBS;
        }
    }

    // Coherent 64-bit read of CC40..CC43. The top three slices are read, then the low one, and
    // retried while any upper slice changed across the low read: a carry crossing ANY boundary
    // flips one of the re-verified words, so the snapshot cannot straddle a wrap. Needs no IRQ
    // save.
    uint64_t ccu4_ticks()
    {
        uint32_t s3;
        uint32_t s2;
        uint32_t s1;
        uint32_t lo;
        do
        {
            s3 = cc4(3, ccu4::slice_off::TIMER) & 0xFFFFu;
            s2 = cc4(2, ccu4::slice_off::TIMER) & 0xFFFFu;
            s1 = cc4(1, ccu4::slice_off::TIMER) & 0xFFFFu;
            lo = cc4(0, ccu4::slice_off::TIMER) & 0xFFFFu;
        } while ((cc4(1, ccu4::slice_off::TIMER) & 0xFFFFu) != s1
                 or (cc4(2, ccu4::slice_off::TIMER) & 0xFFFFu) != s2
                 or (cc4(3, ccu4::slice_off::TIMER) & 0xFFFFu) != s3);
        return (static_cast<uint64_t>(s3) << 48)
               | (static_cast<uint64_t>(s2) << 32)
               | (static_cast<uint64_t>(s1) << 16)
               | lo;
    }

    // Lets a K2DIV step settle (~50 us).
    void clock_delay()
    {
        // Volatile so -Os keeps the wait.
        for (volatile uint32_t i = 0; i < 8000u;)
        {
            i = i + 1;
            __asm volatile("nop");
        }
    }

    // Written ONLY at a rate edge: init() in arch_init and reprice() in arch_cpu_clock_set. The
    // rate is fCCU = fSYS = SystemCoreClock.
    kickos::arch_clk_anchor g_clk;

    // Flash read wait-states: an fCPU increase past the current setting's access window makes a
    // fetch return before the data is valid, a fault and not merely wrong timing (RM 8.4.4).
    // Widen before a rise, relax after a fall.
    void set_flash_ws(uint32_t ws)
    {
        uint32_t fcon = r32(flash::FCON);
        fcon &= ~flash::FCON_WSPFLASH_MASK;
        fcon |= (ws << 0) & flash::FCON_WSPFLASH_MASK;
        r32(flash::FCON) = fcon;
    }

    // Walks K2DIV one step at a time: a large K2DIV DECREASE (a frequency RISE) draws a current
    // step the core supply cannot service in one edge (a VDDC droop), so never jump. The PLL
    // stays locked across a K2DIV change.
    void pll_k2div_staircase(uint32_t from_k2, uint32_t to_k2)
    {
        uint32_t k = from_k2;
        while (k > to_k2)
        {
            k--;
            r32(scu::PLLCON1) = pllcon1_value(k);
            clock_delay();
        }
        while (k < to_k2)
        {
            k++;
            r32(scu::PLLCON1) = pllcon1_value(k);
            clock_delay();
        }
    }

    void clock_init()
    {
        // Wait-states MUST be raised BEFORE the CPU clock scales up, else flash fetches fault.
        uint32_t fcon = r32(flash::FCON);
        fcon &= ~flash::FCON_WSPFLASH_MASK;
        fcon |= flash::FCON_WSPFLASH_4CYC;
        r32(flash::FCON) = fcon;

        // Bring-up transients must not trap.
        uint32_t traps = scu::TRAP_SOSCWDGT | scu::TRAP_SVCOLCKT | scu::TRAP_UVCOLCKT;
        r32(scu::TRAPDIS) |= traps;
        r32(scu::TRAPCLR) = traps;

        r32(scu::PLLCON0) &= ~(scu::PLLCON0_VCOPWD | scu::PLLCON0_PLLPWD);

        if ((r32(scu::OSCHPCTRL) & scu::OSCHPCTRL_MODE_MASK) != 0)
        {
            uint32_t osc = r32(scu::OSCHPCTRL);
            osc &= ~(scu::OSCHPCTRL_MODE_MASK | scu::OSCHPCTRL_OSCVAL_MASK);
            osc |= scu::OSCHPCTRL_OSCVAL;
            r32(scu::OSCHPCTRL) = osc;

            r32(scu::PLLCON2) &= ~scu::PLLCON2_PINSEL;
            r32(scu::PLLCON0) &= ~scu::PLLCON0_OSCRES;

            if (not kickos::arch::wait_set(r32(scu::PLLSTAT), scu::PLLSTAT_OSC_USABLE))
            {
                SystemCoreClock = 24000000u; // no usable crystal: the CPU stays on fOFI
                return;
            }
            r32(scu::TRAPDIS) &= ~scu::TRAP_SOSCWDGT;
        }

        // Lock first at a low K2DIV, then ramp.
        r32(scu::PLLCON0) |= scu::PLLCON0_VCOBYP;
        r32(scu::PLLCON0) |= scu::PLLCON0_FINDIS;
        r32(scu::PLLCON1) = pllcon1_value(12); // 288/12 = 24 MHz
        r32(scu::PLLCON0) |= scu::PLLCON0_OSCDISCDIS;
        r32(scu::PLLCON0) &= ~scu::PLLCON0_FINDIS;
        r32(scu::PLLCON0) |= scu::PLLCON0_RESLD;

        if (not kickos::arch::wait_set(r32(scu::PLLSTAT), scu::PLLSTAT_VCOLOCK))
        {
            SystemCoreClock = 24000000u; // PLL never locked: the CPU stays on fOFI
            return;
        }

        r32(scu::PLLCON0) &= ~scu::PLLCON0_VCOBYP;
        (void)kickos::arch::wait_clear(r32(scu::PLLSTAT), scu::PLLSTAT_VCOBYST);
        // Re-arm the trap for the system VCO, not the USB VCO (never powered here).
        r32(scu::TRAPDIS) &= ~scu::TRAP_SVCOLCKT;

        // fSYS = fPLL, fCPU = fSYS, fPERIPH = fCPU/2.
        r32(scu::SYSCLKCR) = scu::SYSCLKCR_SYSSEL_PLL;
        r32(scu::CPUCLKCR) = 0;
        r32(scu::PBCLKCR) = scu::PBCLKCR_PBDIV_DIV2;

        // Stepped, to avoid a VDDC droop.
        r32(scu::PLLCON0) &= ~scu::PLLCON0_OSCDISCDIS;
        r32(scu::PLLCON1) = pllcon1_value(6); clock_delay(); // 288/6 = 48 MHz
        r32(scu::PLLCON1) = pllcon1_value(4); clock_delay(); // 288/4 = 72 MHz
        r32(scu::PLLCON1) = pllcon1_value(3); clock_delay(); // 288/3 = 96 MHz
        r32(scu::PLLCON1) = pllcon1_value(2); clock_delay(); // 288/2 = 144 MHz

        // fPLL stays the system clock through SLEEP, or a post-print WFI rescales the USIC baud
        // mid-shift.
        r32(scu::SLEEPCR) |= scu::SLEEPCR_SYSSEL_PLL;
    }

    // Privileged-write allowlist for KOS_SYS_PERIPH_REG_WRITE, keyed on the EXACT register
    // address, never on a block or a range.
    //
    // FDR, BRG and CCR are Write = PV with no U (RM Table 18-20): an unprivileged store inside a
    // granted channel window is discarded with no fault (measured, pvprobe).
    //
    // U0C0 carries a CCR entry ONLY (the console driver's TBIEN): the kernel owns that channel's
    // baud (usic_uart.cc), which cpu_clock_set's retune veto assumes. An absent entry is a refusal.
    struct PrivWriteReg
    {
        uintptr_t base;
        uintptr_t offset;
        uint32_t mask; // the only bits this entry may set; see the grants below
    };

    namespace ru = kickos::xmc::reg::usic;

    static_assert(ru::U0C1_BASE == mmap::USIC0_CH0_BASE + USIC_CHANNEL_STRIDE,
                  "U0C1 is USIC0's second channel");

    // Per-entry value masks. The store is one whole 32-bit word, so without a mask an entry hands
    // back every bit of a register the bus withholds entirely. A value with a bit outside its
    // entry's mask is REFUSED (-KOS_EINVAL): the seam never masks and never read-modify-writes.
    //
    // CCR: MODE[3:0], RIEN and AIEN (the receive interrupt enables xmcssc arms last). WITHHELD:
    // HPCEN, PM, RSIEN, DLIEN, TSIEN, TBIEN, BRGIEN and the read-only bits.
    constexpr uint32_t CCR_GRANT = ru::CCR_MODE_MASK | ru::CCR_RIEN | ru::CCR_AIEN;
    // The console channel's holder additionally needs TBIEN. BRGIEN stays WITHHELD: the baud
    // generator clocks it, so it free-runs with the driver doing nothing.
    constexpr uint32_t CCR_CONSOLE_GRANT = CCR_GRANT | ru::CCR_TBIEN;
    // FDR: STEP and DM are the whole divider setting. WITHHELD: RESULT (driven by the divider),
    // the read-only bits, and [31:30], which must be written 0.
    constexpr uint32_t FDR_GRANT = ru::FDR_STEP_MASK | ru::FDR_DM_MASK;
    // BRG: every writable field, so this mask is near-meaningless as an isolation bound.
    constexpr uint32_t BRG_GRANT = ~ru::BRG_RESERVED_MASK;

    // A field-mask edit in regs/usic.h cannot silently widen what the seam hands an unprivileged
    // caller.
    static_assert(CCR_GRANT == 0x0000C00Fu, "CCR grant is MODE[3:0]|RIEN|AIEN");
    static_assert(CCR_CONSOLE_GRANT == 0x0000E00Fu, "console CCR grant is MODE[3:0]|TBIEN|RIEN|AIEN");
    static_assert(FDR_GRANT == 0x0000C3FFu, "FDR grant is STEP[9:0]|DM[15:14]");
    static_assert(BRG_GRANT == 0xF3FF7FDBu, "BRG grant is every writable BRG field");

    constexpr PrivWriteReg PRIV_WRITE_REGS[] = {
        { ru::U0C1_BASE, ru::off::FDR, FDR_GRANT },
        { ru::U0C1_BASE, ru::off::BRG, BRG_GRANT },
        { ru::U0C1_BASE, ru::off::CCR, CCR_GRANT },
        { ru::U0C0_BASE, ru::off::CCR, CCR_CONSOLE_GRANT },
    };

    constexpr uint32_t led_omr(bool level)
    {
        if (level)
        {
            return 1u << KICKOS_BOARD_LED_BIT;
        }
        return 1u << (KICKOS_BOARD_LED_BIT + rp::OMR_RESET_SHIFT);
    }

    constexpr bool LED_LIT = KICKOS_BOARD_LED_ACTIVE_LOW == 0;
}

extern "C"
{

void arch_init(void)
{
    // clock_init FIRST: the baud follows the live fPERIPH and SysTick derives from fCPU.
    clock_init();
    ccu4_clock_init();
    g_clk.init(SystemCoreClock);
    kickos_xmc_usic_init();
    kickos_armv7m_init();
}

// Must stay a pure read: deriving the rate here would reprice all elapsed history.
uint64_t arch_clock_now(void)
{
    return g_clk.ns_from(ccu4_ticks());
}

// Retunes fSYS among the locked-PLL points via the K2DIV staircase, folds the re-anchor into the
// rate edge and returns the LANDED Hz. fCCU and fPERIPH both follow SystemCoreClock, so the
// CCU40 clock AND the USIC baud move.
uint64_t arch_cpu_clock_set(uint32_t target)
{
    uint32_t const previous = SystemCoreClock;
    uint32_t want_hz;
    uint32_t want_k2;
    uint32_t want_ws;
    switch (static_cast<kos_pstate_t>(target))
    {
    case KOS_PSTATE_MAX:
    {
        want_hz = 144000000u; want_k2 = 2u; want_ws = 4u; // fPERIPH 72 MHz
        break;
    }
    case KOS_PSTATE_MID:
    {
        want_hz = 96000000u; want_k2 = 3u; want_ws = 3u;  // fPERIPH 48 MHz
        break;
    }
    default: // KOS_PSTATE_LOW
    {
        want_hz = 48000000u; want_k2 = 6u; want_ws = 2u;  // fPERIPH 24 MHz
        break;
    }
    }
    if (want_hz == previous)
    {
        return previous;
    }
    // Only BETWEEN the known locked-PLL points: a boot that fell back to fOFI is left untouched,
    // a K2DIV staircase off a bypassed PLL being invalid.
    uint32_t cur_k2;
    if (previous == 144000000u) { cur_k2 = 2u; }
    else if (previous == 96000000u) { cur_k2 = 3u; }
    else if (previous == 48000000u) { cur_k2 = 6u; }
    else { return previous; }

    // Capture AT the edge: history is priced at the OLD rate before it moves.
    uint64_t const t0 = ccu4_ticks();
    uint64_t const ns0 = g_clk.ns_from(t0);

    if (want_hz > previous)
    {
        // Wait-states BEFORE the frequency climbs; every intermediate point is <= want_hz, so
        // want_ws covers them all.
        set_flash_ws(want_ws);
        pll_k2div_staircase(cur_k2, want_k2);
    }
    else
    {
        // Frequency first, then relax the wait-states: the old ones are safe across the descent.
        pll_k2div_staircase(cur_k2, want_k2);
        set_flash_ws(want_ws);
    }

    SystemCoreClock = want_hz;

    // Ticks in the brief masked staircase are the only ones mispriced.
    g_clk.reprice(t0, ns0, want_hz);
    return want_hz;
}

// Every USIC runs at fPERIPH = fCPU / PBCLKCR.PBDIV. BOTH TERMS ARE READ LIVE: clock_init
// writes PBCLKCR only after its two degrade returns, so on the fOFI path PBDIV is still 0 and
// fPERIPH equals fCPU. Any other block returns 0.
uint32_t arch_periph_clock_hz(uintptr_t base)
{
    bool in_usic = base >= mmap::USIC0_CH0_BASE and base < mmap::USIC0_CH0_BASE + USIC_MODULE_SPAN;
    if (base >= mmap::USIC1_CH0_BASE and base < mmap::USIC1_CH0_BASE + USIC_MODULE_SPAN)
    {
        in_usic = true;
    }
    if (base >= mmap::USIC2_CH0_BASE and base < mmap::USIC2_CH0_BASE + USIC_MODULE_SPAN)
    {
        in_usic = true;
    }
    if (in_usic)
    {
        if ((r32(scu::PBCLKCR) & scu::PBCLKCR_PBDIV_DIV2) != 0u)
        {
            return SystemCoreClock / 2u;
        }
        return SystemCoreClock;
    }
    return 0;
}

// The syscall layer has checked the caller holds the block at `base`; this decides whether that
// exact register is on the allowlist AND the value stays inside the entry's mask, so a holder of
// one channel window can never name the sibling channel's registers.
//
// This definition MUST stay in this TU because the member is ALWAYS ANCHORED (-u g_isr_vector
// reaches it through startup.S): in a TU nothing else references, the declining fallback in
// arch/common would answer the reference with NO link error and a silent decline at runtime. The
// seam_defaults gate (leg 2) catches that.
int arch_periph_reg_write(uintptr_t base, uintptr_t offset, uint32_t value)
{
    for (PrivWriteReg const& e : PRIV_WRITE_REGS)
    {
        if (e.base == base and e.offset == offset)
        {
            if ((value & ~e.mask) != 0)
            {
                return -KOS_EINVAL;
            }
            r32(base + offset) = value;
            return 0;
        }
    }
    return -KOS_EINVAL;
}

bool arch_console_write_sync(char const* buf, size_t n)
{
    return kickos_xmc_usic_write(buf, n);
}

void arch_diag_led_init(void)
{
    uintptr_t const iocr = KICKOS_BOARD_LED_PORT_BASE + rp::iocr_off(KICKOS_BOARD_LED_BIT);
    r32(iocr) = rp::PC_OUTPUT_PP_GP << rp::pc_shift(KICKOS_BOARD_LED_BIT);
}

void arch_diag_led_set(int on)
{
    uintptr_t const omr = KICKOS_BOARD_LED_PORT_BASE + rp::off::OMR;
    if (on)
    {
        r32(omr) = led_omr(LED_LIT);
    }
    else
    {
        r32(omr) = led_omr(not LED_LIT);
    }
}

// KOS_SYS_PINMUX_SET. func is the raw 5-bit PC code (0x10 = output push-pull general-purpose;
// 0x12 = push-pull alt-func-2, the console TX).
int arch_pinmux_set(uint32_t port, uint32_t pin, uint32_t func)
{
    if (port > 15u or pin > 15u or func > rp::PC_FIELD_MASK)
    {
        return -KOS_EINVAL;
    }
    if (kickos::board_pin_kernel_owned(port, pin))
    {
        return -KOS_EBUSY;
    }
    uintptr_t const iocr = rp::base(port) + rp::iocr_off(pin);
    uint32_t const shift = rp::pc_shift(pin);
    uint32_t v = r32(iocr);
    v &= ~(rp::PC_FIELD_MASK << shift);
    v |= (func & rp::PC_FIELD_MASK) << shift;
    r32(iocr) = v;
    return 0;
}

int arch_bitband_present(void)
{
    return 1;
}

void Reset_Handler(void)
{
    kickos_armv7m_enable_fpu();
    r32(kickos::arm::SCB_VTOR) = mmap::FLASH_CACHED_BASE;

    kickos_ranges_init();
    for (void (**fn)() = __init_array_start; fn != __init_array_end; fn++)
    {
        (*fn)();
    }
    arch_init();
    kickos::kmain(0, nullptr);
    arch_shutdown(0);
}

}
