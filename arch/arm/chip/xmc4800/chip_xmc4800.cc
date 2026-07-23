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

#include <kickos/arch/arch.h>
#include <kickos/arch/clk_q32.h> // shared Q32 tickless-clock reciprocal + multiply
#include <kickos/console_tx.h>

#include <kickos/sys/abi.h> // kos_pstate_t / KOS_PSTATE_* (clock-select)

#include <stdint.h>

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
    constexpr uintptr_t FLASH_BASE = 0x08000000; // cached flash alias (link base)

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

    // SCU sub-unit registers (RM SCU chapter, bases: TRAP 0x50004160, CLK 0x50004600,
    // OSC 0x50004700, PLL 0x50004710).
    constexpr uintptr_t SCU_TRAPDIS = 0x50004168;   // TRAP + 0x08
    constexpr uintptr_t SCU_TRAPCLR = 0x5000416C;   // TRAP + 0x0C
    constexpr uintptr_t SCU_SYSCLKCR = 0x5000460C;  // CLK  + 0x0C
    constexpr uintptr_t SCU_CPUCLKCR = 0x50004610;  // CLK  + 0x10
    constexpr uintptr_t SCU_PBCLKCR = 0x50004614;   // CLK  + 0x14
    constexpr uintptr_t SCU_SLEEPCR = 0x50004630;   // CLK  + 0x30
    constexpr uintptr_t SCU_OSCHPCTRL = 0x50004704; // OSC  + 0x04
    constexpr uintptr_t SCU_PLLSTAT = 0x50004710;   // PLL  + 0x00
    constexpr uintptr_t SCU_PLLCON0 = 0x50004714;   // PLL  + 0x04
    constexpr uintptr_t SCU_PLLCON1 = 0x50004718;   // PLL  + 0x08
    constexpr uintptr_t SCU_PLLCON2 = 0x5000471C;   // PLL  + 0x0C
    // FLASH FCON (PMU/FLASH0 0x58001000 + 0x1014 = 0x58002014; RM flash chapter).
    constexpr uintptr_t FLASH_FCON = 0x58002014;

    // TRAP bits (RM): OSC watchdog, system/USB VCO-lock traps.
    constexpr uint32_t TRAP_SOSCWDGT = 1u << 0;
    constexpr uint32_t TRAP_SVCOLCKT = 1u << 2;
    constexpr uint32_t TRAP_UVCOLCKT = 1u << 3;

    // OSCHPCTRL (RM): MODE[5:4]=0 -> external crystal; OSCVAL[19:16].
    constexpr uint32_t OSCHPCTRL_MODE_MASK = 3u << 4;
    constexpr uint32_t OSCHPCTRL_OSCVAL_MASK = 15u << 16;
    // OSCVAL = XTAL/FOSCREF - 1 = 12e6/2.5e6 - 1 = 3.
    constexpr uint32_t OSCHPCTRL_OSCVAL = 3u << 16;

    // PLLSTAT (RM): crystal usable = PLLHV|PLLLV|PLLSP.
    constexpr uint32_t PLLSTAT_VCOBYST = 1u << 0;
    constexpr uint32_t PLLSTAT_VCOLOCK = 1u << 2;
    constexpr uint32_t PLLSTAT_OSC_USABLE = (1u << 7) | (1u << 8) | (1u << 9);

    // PLLCON0 (RM).
    constexpr uint32_t PLLCON0_VCOBYP = 1u << 0;
    constexpr uint32_t PLLCON0_VCOPWD = 1u << 1;
    constexpr uint32_t PLLCON0_FINDIS = 1u << 4;
    constexpr uint32_t PLLCON0_OSCDISCDIS = 1u << 6;
    constexpr uint32_t PLLCON0_PLLPWD = 1u << 16;
    constexpr uint32_t PLLCON0_OSCRES = 1u << 17;
    constexpr uint32_t PLLCON0_RESLD = 1u << 18;

    // PLLCON2.PINSEL=0 selects OSC_HP (crystal) as the PLL input (RM).
    constexpr uint32_t PLLCON2_PINSEL = 1u << 0;

    // SYSCLKCR.SYSSEL(16)=1 -> fSYS from fPLL; SYSDIV[7:0]=0 -> /1 (RM).
    constexpr uint32_t SYSCLKCR_SYSSEL_PLL = 1u << 16;
    // PBCLKCR.PBDIV(0)=1 -> fPERIPH = fCPU/2 (RM).
    constexpr uint32_t PBCLKCR_PBDIV_DIV2 = 1u << 0;
    // SLEEPCR.SYSSEL(0): 0 = fOFI as system clock in SLEEP, 1 = fPLL (RM p.11-169).
    // The reset value is 0, so a WFI would drop fSYS to fOFI and rescale the USIC
    // baud mid-shift -- selecting fPLL here keeps peripherals at speed while asleep.
    constexpr uint32_t SLEEPCR_SYSSEL_PLL = 1u << 0;

    // FCON.WSPFLASH[3:0] is the flash read wait-state count in fCPU cycles (RM
    // 8.4.4 formula 8.1: WSPFLASH x (1/fCPU) >= ta; fields 0 and 1 both mean one
    // cycle). At 144 MHz field 4 gives a 27.8 ns access window, covering the data
    // sheet ta (~22 ns) -- the same value Infineon's XMCLib ships for its 144 MHz
    // profile, so this is UNCHANGED from the 120 MHz profile.
    constexpr uint32_t FCON_WSPFLASH_MASK = 15u << 0;
    constexpr uint32_t FCON_WSPFLASH_4CYC = 4u << 0;

    // 144 MHz profile: fVCO = 12 MHz * NDIV/PDIV = 12*24/1 = 288 MHz;
    // fPLL = fVCO/K2DIV = 288/2 = 144 MHz. fVCO=288 is the VCO frequency Infineon's
    // XMCLib uses for this part's 144 MHz profile. PLLCON1 fields NDIV[14:8],
    // K2DIV[22:16], PDIV[27:24], each written as (value-1) (RM PLLCON1 fields).
    constexpr uint32_t PLL_NDIV = 24;
    constexpr uint32_t PLL_PDIV = 1;

    uint32_t pllcon1_value(uint32_t k2div)
    {
        return ((PLL_NDIV - 1u) << 8) | ((k2div - 1u) << 16) | ((PLL_PDIV - 1u) << 24);
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
    constexpr uintptr_t SCU_CLKSET = 0x50004604;   // CLK + 0x04
    constexpr uintptr_t SCU_CCUCLKCR = 0x50004620; // CLK + 0x20
    constexpr uintptr_t SCU_CGATCLR0 = 0x50004648; // CLK + 0x48 (peripheral clock gating clear)
    constexpr uintptr_t SCU_PRCLR0 = 0x50004414;   // RCU + 0x14 (peripheral reset clear)
    constexpr uint32_t CLKSET_CCUCEN = 1u << 4;    // enable fCCU generation
    constexpr uint32_t CCUCLKCR_CCUDIV = 1u << 0;  // 0 -> fCCU = fSYS
    constexpr uint32_t CGAT_CCU40 = 1u << 2;       // CCU40 clock-gate bit
    constexpr uint32_t PR_CCU40 = 1u << 2;         // CCU40 reset bit

    constexpr uintptr_t CCU40_BASE = 0x4000C000;
    constexpr uintptr_t CCU40_GIDLC = CCU40_BASE + 0x00C; // global idle clear
    constexpr uintptr_t CCU40_GCSS = CCU40_BASE + 0x010;  // global channel set (shadow transfer)
    constexpr uintptr_t CCU4_SLICE0 = CCU40_BASE + 0x100; // CC40; slices are 0x100 apart
    constexpr uintptr_t CCU4_SLICE_STRIDE = 0x100;
    constexpr uintptr_t CC4_CMC = 0x004;   // TCE @bit20 (concatenation enable)
    constexpr uintptr_t CC4_TCSET = 0x00C; // TRBS @bit0
    constexpr uintptr_t CC4_TC = 0x014;    // counting mode (0 = edge-aligned up-count)
    constexpr uintptr_t CC4_PSC = 0x024;   // prescaler (0 = fCCU/1)
    constexpr uintptr_t CC4_PRS = 0x034;   // shadow period
    constexpr uintptr_t CC4_TIMER = 0x070; // current 16-bit slice value
    constexpr uint32_t GIDLC_SPRB = 1u << 8;    // start the module prescaler
    constexpr uint32_t GIDLC_CLEAR_ALL = 0xFu;  // CS0I..CS3I: clear idle, slices 0-3
    constexpr uint32_t GCSS_SHADOW_ALL =        // S0SE|S1SE|S2SE|S3SE
        (1u << 0) | (1u << 4) | (1u << 8) | (1u << 12);
    constexpr uint32_t CMC_TCE = 1u << 20;    // count the previous slice's overflow
    constexpr uint32_t TCSET_TRBS = 1u << 0;  // set the slice run bit
    constexpr uint32_t CC4_PERIOD_MAX = 0xFFFFu;
    // SLEEPCR.CCUCR (RM SCU): keep fCCU running while the core is in SLEEP. The idle
    // path is a plain WFI (SLEEP, not DEEPSLEEP), and by default the CCU clock gates
    // off in SLEEP -- which freezes this counter on every tickless idle, so a sleep
    // deadline is only ever approached during the brief wake windows and a 40 ms sleep
    // stretches into tens of seconds. (A future DEEPSLEEP user must set DSLEEPCR too.)
    constexpr uint32_t SLEEPCR_CCUCR = 1u << 20;

    inline volatile uint32_t& cc4(unsigned slice, uintptr_t reg)
    {
        return r32(CCU4_SLICE0 + slice * CCU4_SLICE_STRIDE + reg);
    }

    void ccu4_clock_init()
    {
        // Boot-order constraint: arch_clock_now MUST NOT run before this -- CCU40 is
        // clock-gated + in reset out of SCU reset, so a TIMER read would BusFault.
        r32(SCU_CLKSET) = CLKSET_CCUCEN;        // fCCU on (fSYS-derived)
        r32(SCU_CCUCLKCR) &= ~CCUCLKCR_CCUDIV;  // CCUDIV=0 -> fCCU = fSYS = SystemCoreClock
        r32(SCU_CGATCLR0) = CGAT_CCU40;         // ungate the CCU40 module clock
        r32(SCU_PRCLR0) = PR_CCU40;             // release the CCU40 peripheral reset
        r32(SCU_SLEEPCR) |= SLEEPCR_CCUCR;      // keep fCCU alive through WFI idle

        // Clear idle for all four slices AND start the module prescaler in one write;
        // without SPRB the slice clock never runs and every TIMER stays 0.
        r32(CCU40_GIDLC) = GIDLC_SPRB | GIDLC_CLEAR_ALL;

        for (unsigned s = 0; s < 4; s++)
        {
            cc4(s, CC4_TC) = 0;               // edge-aligned up-count, no external events
            cc4(s, CC4_PSC) = 0;              // CC40 = fCCU/1; the linked slices ignore PSC
            cc4(s, CC4_PRS) = CC4_PERIOD_MAX; // wrap at 0xFFFF so each slice carries the next
        }
        // Concatenate: CC41/CC42/CC43 count the overflow of the slice below; CC40 (the
        // 16 LSBs) must keep TCE=0 and counts fCCU directly (RM 23.2.9).
        cc4(0, CC4_CMC) = 0;
        cc4(1, CC4_CMC) = CMC_TCE;
        cc4(2, CC4_CMC) = CMC_TCE;
        cc4(3, CC4_CMC) = CMC_TCE;

        r32(CCU40_GCSS) = GCSS_SHADOW_ALL; // transfer PRS -> PR for every slice

        for (unsigned s = 0; s < 4; s++)
        {
            cc4(s, CC4_TCSET) = TCSET_TRBS; // set every slice run bit
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
            s3 = cc4(3, CC4_TIMER) & 0xFFFFu;
            s2 = cc4(2, CC4_TIMER) & 0xFFFFu;
            s1 = cc4(1, CC4_TIMER) & 0xFFFFu;
            lo = cc4(0, CC4_TIMER) & 0xFFFFu;
        } while ((cc4(1, CC4_TIMER) & 0xFFFFu) != s1
                 or (cc4(2, CC4_TIMER) & 0xFFFFu) != s2
                 or (cc4(3, CC4_TIMER) & 0xFFFFu) != s3);
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
        uint32_t fcon = r32(FLASH_FCON);
        fcon &= ~FCON_WSPFLASH_MASK;
        fcon |= (ws << 0) & FCON_WSPFLASH_MASK;
        r32(FLASH_FCON) = fcon;
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
            r32(SCU_PLLCON1) = pllcon1_value(k);
            clock_delay();
        }
        while (k < to_k2)
        {
            k++;
            r32(SCU_PLLCON1) = pllcon1_value(k);
            clock_delay();
        }
    }

    void clock_init()
    {
        // Flash read wait-states MUST be raised to the 120 MHz value BEFORE the
        // CPU clock is scaled up, else instruction fetches from flash fault (RM
        // flash chapter: widen the access window before increasing fCPU).
        uint32_t fcon = r32(FLASH_FCON);
        fcon &= ~FCON_WSPFLASH_MASK;
        fcon |= FCON_WSPFLASH_4CYC;
        r32(FLASH_FCON) = fcon;

        // Disable + clear the OSC/VCO-lock traps so bring-up transients don't trap.
        uint32_t traps = TRAP_SOSCWDGT | TRAP_SVCOLCKT | TRAP_UVCOLCKT;
        r32(SCU_TRAPDIS) |= traps;
        r32(SCU_TRAPCLR) = traps;

        // Power up the PLL (clear VCO + PLL power-down).
        r32(SCU_PLLCON0) &= ~(PLLCON0_VCOPWD | PLLCON0_PLLPWD);

        // Enable OSC_HP on the 12 MHz crystal unless it is already in XTAL mode.
        if ((r32(SCU_OSCHPCTRL) & OSCHPCTRL_MODE_MASK) != 0)
        {
            uint32_t osc = r32(SCU_OSCHPCTRL);
            osc &= ~(OSCHPCTRL_MODE_MASK | OSCHPCTRL_OSCVAL_MASK);
            osc |= OSCHPCTRL_OSCVAL; // MODE=0 external crystal + OSCVAL=3
            r32(SCU_OSCHPCTRL) = osc;

            r32(SCU_PLLCON2) &= ~PLLCON2_PINSEL; // PLL input <- OSC_HP
            r32(SCU_PLLCON0) &= ~PLLCON0_OSCRES; // restart OSC watchdog

            if (not clock_wait_set(SCU_PLLSTAT, PLLSTAT_OSC_USABLE))
            {
                SystemCoreClock = 24000000u; // no usable crystal -> CPU stays on fOFI
                return;                       // SysTick tracks it; USIC baud will be off
            }
            r32(SCU_TRAPDIS) &= ~TRAP_SOSCWDGT;
        }

        // Bypass + disconnect the VCO, program the dividers, reconnect, relock.
        // Lock first at a low K2DIV (~24 MHz), then ramp down to 144 MHz below.
        r32(SCU_PLLCON0) |= PLLCON0_VCOBYP;
        r32(SCU_PLLCON0) |= PLLCON0_FINDIS;
        r32(SCU_PLLCON1) = pllcon1_value(12); // 288/12 = 24 MHz
        r32(SCU_PLLCON0) |= PLLCON0_OSCDISCDIS;
        r32(SCU_PLLCON0) &= ~PLLCON0_FINDIS;
        r32(SCU_PLLCON0) |= PLLCON0_RESLD;

        if (not clock_wait_set(SCU_PLLSTAT, PLLSTAT_VCOLOCK))
        {
            SystemCoreClock = 24000000u; // PLL never locked -> CPU stays on fOFI
            return;                      // SysTick tracks it; USIC baud will be off
        }

        // Leave bypass: fPLL drives the tree; wait for normal (non-bypass) mode.
        r32(SCU_PLLCON0) &= ~PLLCON0_VCOBYP;
        clock_wait_clear(SCU_PLLSTAT, PLLSTAT_VCOBYST);
        // Re-arm the trap for the PLL we just locked (system VCO), not the USB VCO
        // (never powered here); SOSCWDGT was already re-armed above.
        r32(SCU_TRAPDIS) &= ~TRAP_SVCOLCKT;

        // Clock dividers: fSYS = fPLL/1, fCPU = fSYS/1, fPERIPH = fCPU/2 = 72 MHz.
        r32(SCU_SYSCLKCR) = SYSCLKCR_SYSSEL_PLL; // fPLL selected, SYSDIV /1
        r32(SCU_CPUCLKCR) = 0;                   // CPUDIV disabled -> fCPU = fSYS
        r32(SCU_PBCLKCR) = PBCLKCR_PBDIV_DIV2;   // fPERIPH = fCPU/2

        // Ramp K2DIV down to the final 144 MHz in steps to avoid a VDDC droop on
        // a large jump (K2DIV = fVCO/target).
        r32(SCU_PLLCON0) &= ~PLLCON0_OSCDISCDIS;
        r32(SCU_PLLCON1) = pllcon1_value(6); clock_delay(); // 288/6 = 48 MHz
        r32(SCU_PLLCON1) = pllcon1_value(4); clock_delay(); // 288/4 = 72 MHz
        r32(SCU_PLLCON1) = pllcon1_value(3); clock_delay(); // 288/3 = 96 MHz
        r32(SCU_PLLCON1) = pllcon1_value(2); clock_delay(); // 288/2 = 144 MHz

        // Keep the system clock on fPLL through SLEEP so a post-print WFI does not
        // rescale the USIC baud mid-shift (see SLEEPCR_SYSSEL_PLL).
        r32(SCU_SLEEPCR) |= SLEEPCR_SYSSEL_PLL;
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
    constexpr uintptr_t P5_IOCR8 = 0x48028500 + 0x18;
    r32(P5_IOCR8) = 0x10u << 11; // PC9 = 0b10000 (output push-pull), bits [15:11]
}

void arch_diag_led_set(int on)
{
    constexpr uintptr_t P5_OMR = 0x48028500 + 0x04;
    if (on)
    {
        r32(P5_OMR) = 1u << 9;
    }
    else
    {
        r32(P5_OMR) = 1u << (9 + 16);
    }
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
        {0x4000C000u, 0x1000u}, // CCU40: timebase slice + global control (RM ch.23)
        {0x50004000u, 0x1000u}, // SCU: CGATSET clock gates / PRSET resets / PLL (RM SCU ch.)
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
    r32(SCB_VTOR) = FLASH_BASE; // vectors live at the cached flash alias

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
