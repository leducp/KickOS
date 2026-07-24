// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Infineon XMC4800 (XMC4800 Relax Kit, Cortex-M4F) chip backend. Registers
// clean-room from the XMC4700/XMC4800 Reference Manual; hand-rolled, no XMCLib.
//
// M1 scope: privilege + SVC, no MPU. The watchdog is OFF at reset (WDT_CTR.ENB
// = 0), so the reset path is just FPU + C-runtime + VTOR. arch_init then runs
// clock_init() to bring the SCU up from the 12 MHz crystal PLL to fCPU=144 MHz
// (fPERIPH=72 MHz) -- the uncalibrated fOFI (~24 MHz) is too inaccurate for a
// stable UART baud. Code/vectors are linked at the cached flash alias
// 0x0800_0000.
//
// Console: USIC0 in ASC (UART) mode on P1.5/P1.4 -> the on-board J-Link VCOM
// (ttyACM0) at 115200; see usic_uart.cc. apps/blink toggles LED1 (P5.9). The
// XMC4800 also carries an on-chip EtherCAT node, a natural future KickCAT target.
//
// Build-only here; flash via the on-board debugger.

#include "mmap.h"
#include "regs/ccu4.h"
#include "regs/flash.h"
#include "regs/port.h"
#include "regs/scu.h"

#include <kickos/arch/arch.h>
#include <kickos/arch/clk_q32.h> // shared Q32 tickless-clock reciprocal + multiply
#include <kickos/console_tx.h>

#include <kickos/sys/abi.h> // kos_pstate_t / KOS_PSTATE_* (clock-select)

#include <stdint.h>

namespace mmap = kickos::xmc::mmap;
namespace scu = kickos::xmc::reg::scu;
namespace ccu4 = kickos::xmc::reg::ccu4;
namespace flash = kickos::xmc::reg::flash;
namespace rp = kickos::xmc::reg::port;

namespace kickos
{
    int kmain(int argc, char** argv);
}

extern "C"
{
    void kickos_armv7m_init(void);
    void kickos_xmc_usic_init(void);                        // usic_uart.cc
    void kickos_xmc_usic_write(char const* buf, size_t n);  // usic_uart.cc

    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();

    uint32_t SystemCoreClock = 144000000u; // fCPU after clock_init (drives SysTick)
}

namespace
{
    inline volatile uint32_t& r32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }

    constexpr uintptr_t SCB_VTOR = 0xE000ED08;

    void enable_fpu()
    {
        r32(0xE000ED88) |= (0xFu << 20); // CPACR: CP10/CP11 full access
        __asm volatile("dsb" ::: "memory");
        __asm volatile("isb" ::: "memory");
    }

    // ---- Clock tree: 12 MHz XTAL -> system PLL -> fSYS=fCPU=144 MHz -----------
    // Sequence and register values are clean-room from the XMC4700/4800 RM V1.3
    // (SCU clock/PLL chapter + flash chapter). The 144 MHz profile -- NDIV/PDIV/
    // K2DIV and the staged K2DIV ramp -- is the RM's crystal-PLL bring-up for
    // this part; register addresses/fields are the RM's SCU/FLASH/memory-map values.

    // 144 MHz profile: fVCO = 12 MHz * NDIV/PDIV = 12*24/1 = 288 MHz;
    // fPLL = fVCO/K2DIV = 288/2 = 144 MHz. Each PLLCON1 field is written as (value-1).
    uint32_t pllcon1_value(uint32_t k2div)
    {
        return ((scu::PLL_NDIV - 1u) << scu::PLLCON1_NDIV_SHIFT)
             | ((k2div - 1u) << scu::PLLCON1_K2DIV_SHIFT)
             | ((scu::PLL_PDIV - 1u) << scu::PLLCON1_PDIV_SHIFT);
    }

    // Bounded like the rp2040 wait_mask / the USIC TX poll: a missing crystal
    // degrades (returns) instead of hanging the boot. Cap dwarfs any real wait.
    constexpr uint32_t CLOCK_POLL_TIMEOUT = 1000000u;

    bool clock_wait_set(uintptr_t addr, uint32_t mask)
    {
        for (uint32_t i = 0; i < CLOCK_POLL_TIMEOUT; i++)
        {
            if ((r32(addr) & mask) == mask)
            {
                return true;
            }
        }
        return false;
    }

    void clock_wait_clear(uintptr_t addr, uint32_t mask)
    {
        for (uint32_t i = 0; i < CLOCK_POLL_TIMEOUT; i++)
        {
            if ((r32(addr) & mask) == 0)
            {
                return;
            }
        }
    }

    // --- CCU40: the monotonic time base (RM ch.23) ------------------------------
    // The v7-M default clock is the DWT cycle counter, but on this silicon DWT_CYCCNT
    // reads are unreliable (core debug power domain; observed returning DWT_CTRL's
    // value) and the 32->64 software wrap-extension turns one bad read into a phantom
    // 2^32 jump that strands every timed wait. CCU40's four 16-bit slices are chained
    // (CC41/CC42/CC43 each count the overflow of the slice below) into ONE free-running
    // 64-bit HARDWARE counter on fCCU -- a plain peripheral, not the debug domain --
    // read as arch_clock_now. Being 64-bit in hardware there is NO software wrap word,
    // so no read can manufacture a wrap (the DWT failure mode is structurally absent).
    // ONLY the monotonic clock moves off DWT; arch_trace_now + the KICKOS_BENCH
    // timestamps stay on raw DWT_CYCCNT (a glitch there skews a telemetry sample --
    // tolerable -- but is fatal to the scheduler's monotonic clock, moved here).
    //
    // Register addresses/bits are the RM's SCU + CCU4 values, cross-checked against
    // the XMC4800 CMSIS device header. A previous CCU4 attempt set only CLKSET.CCUCEN
    // and the slices never counted: CCU40 also comes out of SCU reset both CLOCK-GATED
    // (CGATCLR0) and held in PERIPHERAL RESET (PRCLR0), and needs the module prescaler
    // run bit (GIDLC.SPRB) -- all three are required before any slice advances.
    // SLEEPCR.CCUCR (RM SCU): keep fCCU running while the core is in SLEEP. The idle
    // path is a plain WFI (SLEEP, not DEEPSLEEP), and by default the CCU clock gates
    // off in SLEEP -- which freezes this counter on every tickless idle, so a sleep
    // deadline is only ever approached during the brief wake windows and a 40 ms sleep
    // stretches into tens of seconds. (A future DEEPSLEEP user must set DSLEEPCR too.)

    inline volatile uint32_t& cc4(unsigned slice, uintptr_t reg)
    {
        return r32(ccu4::slice(slice, reg));
    }

    void ccu4_clock_init()
    {
        // Boot-order constraint: arch_clock_now MUST NOT run before this -- CCU40 is
        // clock-gated + in reset out of SCU reset, so a TIMER read would BusFault.
        r32(scu::CLKSET) = scu::CLKSET_CCUCEN;          // fCCU on (fSYS-derived)
        r32(scu::CCUCLKCR) &= ~scu::CCUCLKCR_CCUDIV;    // CCUDIV=0 -> fCCU = fSYS = SystemCoreClock
        r32(scu::CGATCLR0) = scu::CCU40_GATE_BIT;       // ungate the CCU40 module clock
        r32(scu::PRCLR0) = scu::CCU40_RESET_BIT;        // release the CCU40 peripheral reset
        r32(scu::SLEEPCR) |= scu::SLEEPCR_CCUCR;        // keep fCCU alive through WFI idle

        // Clear idle for all four slices AND start the module prescaler in one write;
        // without SPRB the slice clock never runs and every TIMER stays 0.
        r32(ccu4::GIDLC) = ccu4::GIDLC_SPRB | ccu4::GIDLC_CLEAR_ALL;

        for (unsigned s = 0; s < 4; s++)
        {
            cc4(s, ccu4::slice_off::TC) = 0;                    // edge-aligned up-count, no external events
            cc4(s, ccu4::slice_off::PSC) = 0;                  // CC40 = fCCU/1; the linked slices ignore PSC
            cc4(s, ccu4::slice_off::PRS) = ccu4::PERIOD_MAX;   // wrap at 0xFFFF so each slice carries the next
        }
        // Concatenate: CC41/CC42/CC43 count the overflow of the slice below; CC40 (the
        // 16 LSBs) must keep TCE=0 and counts fCCU directly (RM 23.2.9).
        cc4(0, ccu4::slice_off::CMC) = 0;
        cc4(1, ccu4::slice_off::CMC) = ccu4::CMC_TCE;
        cc4(2, ccu4::slice_off::CMC) = ccu4::CMC_TCE;
        cc4(3, ccu4::slice_off::CMC) = ccu4::CMC_TCE;

        r32(ccu4::GCSS) = ccu4::GCSS_SHADOW_ALL; // transfer PRS -> PR for every slice

        for (unsigned s = 0; s < 4; s++)
        {
            cc4(s, ccu4::slice_off::TCSET) = ccu4::TCSET_TRBS; // set every slice run bit
        }
    }

    // Coherent 64-bit read of the concatenated CC40..CC43 counter (counts UP). Only
    // CC40 advances every fCCU tick; the upper slices carry rarely. Read the top three
    // slices then the low one, and retry while any upper slice changed across the low
    // read: a carry crossing ANY slice boundary during the window flips one of the
    // re-verified words, so the returned snapshot can never straddle a wrap. Every load
    // re-reads hardware (no cached word), so this is concurrency-safe with no IRQ save.
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

    // Let a K2DIV step settle before the next one. A generous fixed nop count
    // (a per-frequency settle delay; ~50 us is ample).
    void clock_delay()
    {
        for (volatile uint32_t i = 0; i < 8000u; i++)
        {
            __asm volatile("nop");
        }
    }

    // --- arch_clock_now epoch anchor + clock-select re-anchor (B2/S2) -----------
    // ns = base_ns + (raw_ticks - base_ticks)*mult. The SOLE writer of `mult` is
    // clock_anchor_init (boot) and arch_cpu_clock_set (the re-anchor at the rate edge);
    // arch_clock_now only READS. WHY sole-writer: the old lazy `if (hz != cached_hz)
    // recompute` inside arch_clock_now, if it survived, would let any now() called
    // between the SystemCoreClock write and the re-anchor recompute mult itself against
    // the new Hz and bake the phantom forward jump (all history repriced 6x) into
    // base_ns PERMANENTLY.
    uint64_t g_clk_base_ns = 0;
    uint64_t g_clk_base_ticks = 0;
    uint64_t g_clk_mult = 0;

    uint64_t clock_recip(uint32_t hz)
    {
        return kickos::arch_clk_recip_q32(hz);
    }

    // ns from a raw tick count under the CURRENT anchor (used by arch_clock_now AND to
    // capture history at OLD pricing during a re-anchor). 64x64 split as before.
    uint64_t clock_ns_from(uint64_t ticks)
    {
        uint64_t delta = ticks - g_clk_base_ticks;
        return g_clk_base_ns + kickos::arch_clk_mul_q32(delta, g_clk_mult);
    }

    void clock_anchor_init()
    {
        uint32_t const hz = SystemCoreClock; // fCCU = fSYS = SystemCoreClock
        if (hz == 0)
        {
            return;
        }
        g_clk_mult = clock_recip(hz);
        g_clk_base_ticks = 0; // BOOT-IDENTICAL: now = raw_ticks*mult (the old first read)
        g_clk_base_ns = 0;
    }

    // FCON.WSPFLASH[3:0]: flash read wait-states in fCPU cycles. WHY set-before-rise:
    // a raw fCPU increase past the current wait-state's access window makes an
    // instruction fetch from flash return before the data is valid -> a fetch fault,
    // not merely wrong timing (RM 8.4.4). So widen WS before a rise, relax after a fall.
    void set_flash_ws(uint32_t ws)
    {
        uint32_t fcon = r32(flash::FCON);
        fcon &= ~flash::FCON_WSPFLASH_MASK;
        fcon |= (ws << 0) & flash::FCON_WSPFLASH_MASK;
        r32(flash::FCON) = fcon;
    }

    // Walk K2DIV one step at a time from `from` to `to` (fPLL = fVCO/K2DIV = 288/K2DIV),
    // settling between steps. WHY stepwise, never a jump: a large K2DIV DECREASE (a
    // frequency RISE) draws a current step the core supply cannot service in one edge
    // -- a VDDC droop -- so the boot ramp and this retune both stair-step. The PLL stays
    // LOCKED across a K2DIV change (only the output divider moves), so no relock/poll.
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
        // Flash read wait-states MUST be raised to the 120 MHz value BEFORE the
        // CPU clock is scaled up, else instruction fetches from flash fault (RM
        // flash chapter: widen the access window before increasing fCPU).
        uint32_t fcon = r32(flash::FCON);
        fcon &= ~flash::FCON_WSPFLASH_MASK;
        fcon |= flash::FCON_WSPFLASH_4CYC;
        r32(flash::FCON) = fcon;

        // Disable + clear the OSC/VCO-lock traps so bring-up transients don't trap.
        uint32_t traps = scu::TRAP_SOSCWDGT | scu::TRAP_SVCOLCKT | scu::TRAP_UVCOLCKT;
        r32(scu::TRAPDIS) |= traps;
        r32(scu::TRAPCLR) = traps;

        // Power up the PLL (clear VCO + PLL power-down).
        r32(scu::PLLCON0) &= ~(scu::PLLCON0_VCOPWD | scu::PLLCON0_PLLPWD);

        // Enable OSC_HP on the 12 MHz crystal unless it is already in XTAL mode.
        if ((r32(scu::OSCHPCTRL) & scu::OSCHPCTRL_MODE_MASK) != 0)
        {
            uint32_t osc = r32(scu::OSCHPCTRL);
            osc &= ~(scu::OSCHPCTRL_MODE_MASK | scu::OSCHPCTRL_OSCVAL_MASK);
            osc |= scu::OSCHPCTRL_OSCVAL; // MODE=0 external crystal + OSCVAL=3
            r32(scu::OSCHPCTRL) = osc;

            r32(scu::PLLCON2) &= ~scu::PLLCON2_PINSEL; // PLL input <- OSC_HP
            r32(scu::PLLCON0) &= ~scu::PLLCON0_OSCRES; // restart OSC watchdog

            if (not clock_wait_set(scu::PLLSTAT, scu::PLLSTAT_OSC_USABLE))
            {
                SystemCoreClock = 24000000u; // no usable crystal -> CPU stays on fOFI
                return;                       // SysTick tracks it; USIC baud will be off
            }
            r32(scu::TRAPDIS) &= ~scu::TRAP_SOSCWDGT;
        }

        // Bypass + disconnect the VCO, program the dividers, reconnect, relock.
        // Lock first at a low K2DIV (~24 MHz), then ramp down to 144 MHz below.
        r32(scu::PLLCON0) |= scu::PLLCON0_VCOBYP;
        r32(scu::PLLCON0) |= scu::PLLCON0_FINDIS;
        r32(scu::PLLCON1) = pllcon1_value(12); // 288/12 = 24 MHz
        r32(scu::PLLCON0) |= scu::PLLCON0_OSCDISCDIS;
        r32(scu::PLLCON0) &= ~scu::PLLCON0_FINDIS;
        r32(scu::PLLCON0) |= scu::PLLCON0_RESLD;

        if (not clock_wait_set(scu::PLLSTAT, scu::PLLSTAT_VCOLOCK))
        {
            SystemCoreClock = 24000000u; // PLL never locked -> CPU stays on fOFI
            return;                      // SysTick tracks it; USIC baud will be off
        }

        // Leave bypass: fPLL drives the tree; wait for normal (non-bypass) mode.
        r32(scu::PLLCON0) &= ~scu::PLLCON0_VCOBYP;
        clock_wait_clear(scu::PLLSTAT, scu::PLLSTAT_VCOBYST);
        // Re-arm the trap for the PLL we just locked (system VCO), not the USB VCO
        // (never powered here); SOSCWDGT was already re-armed above.
        r32(scu::TRAPDIS) &= ~scu::TRAP_SVCOLCKT;

        // Clock dividers: fSYS = fPLL/1, fCPU = fSYS/1, fPERIPH = fCPU/2 = 72 MHz.
        r32(scu::SYSCLKCR) = scu::SYSCLKCR_SYSSEL_PLL; // fPLL selected, SYSDIV /1
        r32(scu::CPUCLKCR) = 0;                        // CPUDIV disabled -> fCPU = fSYS
        r32(scu::PBCLKCR) = scu::PBCLKCR_PBDIV_DIV2;   // fPERIPH = fCPU/2

        // Ramp K2DIV down to the final 144 MHz in steps to avoid a VDDC droop on
        // a large jump (K2DIV = fVCO/target).
        r32(scu::PLLCON0) &= ~scu::PLLCON0_OSCDISCDIS;
        r32(scu::PLLCON1) = pllcon1_value(6); clock_delay(); // 288/6 = 48 MHz
        r32(scu::PLLCON1) = pllcon1_value(4); clock_delay(); // 288/4 = 72 MHz
        r32(scu::PLLCON1) = pllcon1_value(3); clock_delay(); // 288/3 = 96 MHz
        r32(scu::PLLCON1) = pllcon1_value(2); clock_delay(); // 288/2 = 144 MHz

        // Keep the system clock on fPLL through SLEEP so a post-print WFI does not
        // rescale the USIC baud mid-shift (see scu::SLEEPCR_SYSSEL_PLL).
        r32(scu::SLEEPCR) |= scu::SLEEPCR_SYSSEL_PLL;
    }
}

extern "C"
{

void arch_init(void)
{
    // Scale the SCU from the reset fOFI to the 12 MHz crystal PLL (fCPU=144 MHz,
    // fPERIPH=72 MHz) FIRST: the USIC baud constants are computed for fPERIPH=72
    // MHz, and SysTick derives from fCPU. Then bring up the console; finally
    // kickos_armv7m_init installs the NVIC/SHPR priorities.
    clock_init();
    ccu4_clock_init();   // monotonic time base (see ccu4_clock_init note; replaces DWT)
    clock_anchor_init(); // set the arch_clock_now mult ONCE from the final clock (B2)
    kickos_xmc_usic_init();
    kickos_armv7m_init();
}

// Monotonic clock override: convert free-running CCU40 (64-bit hardware counter on
// fCCU = fSYS) ticks to ns, replacing the weak DWT-backed arch_clock_now (unreliable
// on this silicon). ns = ticks * 1e9 / ccu_hz via a cached reciprocal multiply
// (mult = (1e9<<32)/hz, ns = (ticks*mult)>>32, split 64x64 to avoid overflow); the
// one 64-bit divide runs only when SystemCoreClock changes at boot.
uint64_t arch_clock_now(void)
{
    // Pure epoch read (B2): the mult is written ONLY by clock_anchor_init (boot) and
    // the arch_cpu_clock_set re-anchor -- never recomputed here -- so a read in the
    // window around a retune can never bake the phantom rate jump into the anchor.
    return clock_ns_from(ccu4_ticks());
}

// Clock-select MECHANISM (arch.h): retune fSYS among the locked-PLL points via the
// K2DIV staircase, fold the re-anchor into the rate edge, return the LANDED Hz. The
// generic coherence tail (baud re-derive, SysTick re-arm) runs in cpu_clock_set. Called
// privileged, IRQs already masked (single-core: the timer is quiesced). fCCU=fSYS and
// fPERIPH=fCPU/2 both follow SystemCoreClock, so the CCU40 clock AND the USIC baud move.
uint32_t arch_cpu_clock_set(uint32_t target)
{
    uint32_t const previous = SystemCoreClock;
    uint32_t want_hz;
    uint32_t want_k2;
    uint32_t want_ws;
    switch (static_cast<kos_pstate_t>(target))
    {
    case KOS_PSTATE_MAX:
        want_hz = 144000000u; want_k2 = 2u; want_ws = 4u; // fPERIPH 72 MHz
        break;
    case KOS_PSTATE_MID:
        want_hz = 96000000u; want_k2 = 3u; want_ws = 3u;  // fPERIPH 48 MHz
        break;
    default: // KOS_PSTATE_LOW
        want_hz = 48000000u; want_k2 = 6u; want_ws = 2u;  // fPERIPH 24 MHz
        break;
    }
    if (want_hz == previous)
    {
        return previous; // no move (generic also guards; keep the backend honest)
    }
    // Only retune BETWEEN the known locked-PLL points. A boot that fell back to fOFI
    // (~24 MHz, PLL never locked) or any unexpected state is left untouched -- return
    // the truthful current Hz rather than driving a K2DIV staircase off a bypassed PLL.
    uint32_t cur_k2;
    if (previous == 144000000u) { cur_k2 = 2u; }
    else if (previous == 96000000u) { cur_k2 = 3u; }
    else if (previous == 48000000u) { cur_k2 = 6u; }
    else { return previous; }

    // Re-anchor capture AT the edge: history priced at the OLD mult before it moves.
    uint64_t const t0 = ccu4_ticks();
    uint64_t const ns0 = clock_ns_from(t0);

    if (want_hz > previous)
    {
        // RISE: widen flash wait-states BEFORE the frequency climbs (S3), then walk
        // K2DIV DOWN the staircase (every intermediate point is <= want_hz, so want_ws
        // covers them all).
        set_flash_ws(want_ws);
        pll_k2div_staircase(cur_k2, want_k2);
    }
    else
    {
        // FALL: drop the frequency first (K2DIV UP), THEN relax flash wait-states -- the
        // old (higher) WS is safe across the whole descent.
        pll_k2div_staircase(cur_k2, want_k2);
        set_flash_ws(want_ws);
    }

    SystemCoreClock = want_hz; // fCCU=fSYS and fPERIPH=fCPU/2 both track this now

    // Commit the NEW pricing -- the SOLE writer of mult (B2). base_ns holds history at
    // old pricing, base_ticks the tick at the edge, so `now` is continuous (no jump):
    // ticks in the brief masked staircase are the only ones mispriced (frozen skew).
    g_clk_base_ns = ns0;
    g_clk_base_ticks = t0;
    g_clk_mult = clock_recip(want_hz);
    __asm volatile("" ::: "memory"); // pin the triple write order vs a later now() read
    return want_hz;
}

// Branch-clock oracle (arch.h): report the branch clock feeding a peripheral block
// so a userspace driver derives its own divisor. On the XMC4800 every USIC runs at
// fPERIPH = fCPU/2 (SCU PBCLKCR.PBDIV=1), so the invariant is SystemCoreClock/2
// computed from the LIVE clock (a clock-select retune is auto-reflected, not a baked
// snapshot). Match the three USIC module ranges (each is two 0x200 channels: USIC0
// @0x40030000, USIC1 @0x48020000, USIC2 @0x48024000); any other block is unknown
// here and returns 0 so the driver keeps its explicit fallback.
uint32_t arch_periph_clock_hz(uintptr_t base)
{
    bool in_usic = base >= mmap::USIC0_CH0_BASE and base < mmap::USIC0_CH0_BASE + mmap::USIC_MODULE_SPAN;
    if (base >= mmap::USIC1_CH0_BASE and base < mmap::USIC1_CH0_BASE + mmap::USIC_MODULE_SPAN)
    {
        in_usic = true;
    }
    if (base >= mmap::USIC2_CH0_BASE and base < mmap::USIC2_CH0_BASE + mmap::USIC_MODULE_SPAN)
    {
        in_usic = true;
    }
    if (in_usic)
    {
        return SystemCoreClock / 2u; // fPERIPH = fCPU/2 (PBCLKCR.PBDIV=1)
    }
    return 0;
}

// Native transport = USIC0 ASC on P1.5/P1.4 (the Relax Kit VCOM -> ttyACM0). RTT
// (if KICKOS_CONSOLE=both) is teed by the kernel console core, not here.
//   arch_console_write      -- buffered (console ring drains via the TB interrupt).
//   arch_console_write_sync -- the bounded polled writer; panic/fault/pre-arm use
//                              it (overrides the weak default in console.cc).
void arch_console_write(char const* buf, size_t n)
{
    console_tx_write(buf, n);
}

void arch_console_write_sync(char const* buf, size_t n)
{
    kickos_xmc_usic_write(buf, n);
}

// Kernel diagnostic LED: LED1 = P5.9, active-high. XMC ports are always clocked
// (no per-port gate). OMR is set/reset in one write: PS9 (bit 9) drives high,
// PR9 (bit 9+16) drives low.
void arch_diag_led_init(void)
{
    uintptr_t const P5_IOCR8 = rp::base(5) + rp::iocr_off(9);
    r32(P5_IOCR8) = rp::PC_OUTPUT_PP_GP << rp::pc_shift(9); // PC9 output push-pull
}

void arch_diag_led_set(int on)
{
    uintptr_t const P5_OMR = rp::base(5) + rp::off::OMR;
    if (on)
    {
        r32(P5_OMR) = 1u << 9;
    }
    else
    {
        r32(P5_OMR) = 1u << (9 + rp::OMR_RESET_SHIFT);
    }
}

// Kernel-owned pins arch_pinmux_set refuses so a board map cannot dark the console
// or steal the diag LED. P1.4/P1.5 = console RX/TX; P5.9 = diag LED.
static bool xmc_pin_kernel_owned(uint32_t port, uint32_t pin)
{
    return (port == 1u and (pin == 4u or pin == 5u)) or (port == 5u and pin == 9u);
}

// One-shot pin-function config (KOS_SYS_PINMUX_SET). IOCR address + PC-field encoding
// come from regs/port.h (shared with the console pin setup). func = the raw 5-bit PC
// code (0x10 = output push-pull general-purpose; 0x12 = PP alt-func-2, the console TX).
int arch_pinmux_set(uint32_t port, uint32_t pin, uint32_t func)
{
    if (port > 15u or pin > 15u or func > rp::PC_FIELD_MASK)
    {
        return -KOS_EINVAL;
    }
    if (xmc_pin_kernel_owned(port, pin))
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

void arch_shutdown(int status)
{
    (void)status;
    __asm volatile("cpsid i" ::: "memory");
    while (true)
    {
        __asm volatile("wfi");
    }
}

#if KICKOS_HAVE_MPU
// Rule 7 reserved set (XMC4[78]00 RM). Owns-for-life: the CCU40 monotonic time base
// (its slice + the global-control prefix) and the SCU (clock gates / peripheral
// resets / PLL). Bases are the constants above; sizes are one register block each.
// This is the silicon-validation target -- keep it exact.
size_t arch_reserved_blocks(struct arch_reserved_block* out, size_t max)
{
    static struct arch_reserved_block const blocks[] = {
        {mmap::CCU40_BASE, 0x1000u}, // CCU40: timebase slice + global control (RM ch.23)
        {mmap::SCU_BASE, 0x1000u},   // SCU: CGATSET clock gates / PRSET resets / PLL (RM SCU ch.)
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

// XMC4800 is a Cortex-M4 with the bit-band peripheral/SRAM alias.
int arch_bitband_present(void)
{
    return 1;
}

void Reset_Handler(void)
{
    enable_fpu();
    r32(SCB_VTOR) = mmap::FLASH_CACHED_BASE; // vectors live at the cached flash alias

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
