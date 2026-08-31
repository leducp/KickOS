// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// MK64FN1M0 (FRDM-K64F) chip backend. Register addresses/fields are from the K64
// Sub-Family Reference Manual (K64P144M120SF5RM).
//
// Silicon-risk points to check against the K64 RM if bring-up misbehaves: the
// 50 MHz source is an external CLOCK (EREFS0=0, RANGE0=2), not a crystal;
// PRDIV/VDIV encodings; and the FRDIV /1536 mapping.

#include <kickos/arch/arch.h>
#include <kickos/config/limits.h>
#include <kickos/arch/clk_anchor.h> // shared tickless-clock epoch anchor (B2)
#include <kickos/console_tx.h>

#include <kickos/sys/abi.h> // kos_pstate_t / KOS_PSTATE_* (clock-select)

#include <stdint.h>

#include "regs.h" // arch/arm/common: kickos_armv7m_enable_fpu + core SCB regs
#include <kickos/chip_mmap.h>
#include "irq.h"
#include "regs/aips.h"
#include "regs/gpio.h"
#include "regs/mcg.h"
#include "regs/osc.h"
#include "regs/pit.h"
#include "regs/port.h"
#include "regs/sim.h"
#include "regs/sysmpu.h"
#include "regs/uart.h"
#include "regs/wdog.h"

namespace mmap = kickos::mk64f::mmap;
namespace reg = kickos::mk64f::reg;
namespace irq = kickos::mk64f::irq;

namespace kickos
{
    int kmain(int argc, char** argv);
    void kprintf(char const* fmt, ...);
}

extern "C"
{
    void kickos_armv7m_init(void);

    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();

    // FEI reset clock (MCGOUTCLK = 32.768 kHz internal ref x 640 FLL). This is the
    // initial + fallback value; clock_init() raises it to 120 MHz on success.
    // UART0 and SysTick are clocked by this system clock.
    uint32_t SystemCoreClock = 20971520u;
}

namespace
{
    inline volatile uint32_t& r32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }
    inline volatile uint16_t& r16(uintptr_t a) { return *reinterpret_cast<volatile uint16_t*>(a); }
    inline volatile uint8_t& r8(uintptr_t a) { return *reinterpret_cast<volatile uint8_t*>(a); }

    // Bus clock = core / BUS_DIV. Used by BOTH the OUTDIV2 field below AND the PIT
    // clock rate in arch_clock_now, so retuning the divider cannot silently rescale
    // kernel time.
    constexpr uint32_t BUS_DIV = 2;
    // SIM_CLKDIV1: core /1 (120), bus /2 (60), FlexBus /2 (60), flash /5 (24 MHz;
    // FLASHCLK must stay <= 25 MHz). Field = divide-1, in nibbles [31:16].
    constexpr uint32_t CLKDIV1_120MHZ =
        (0u << 28) | ((BUS_DIV - 1u) << 24) | (1u << 20) | (4u << 16);

    // Bounded: a missing external clock must degrade to the FEI fallback, not hang boot.
    constexpr uint32_t MCG_POLL_TIMEOUT = 1000000u;

    // OpenSDA VCOM is PTB16/PTB17. Per the K64 signal-mux table these pins are
    // UART0_RX/UART0_TX at ALT3 (PTB16 has no UART1 option). The FRDM-K64F user
    // guide's "UART1" label is a doc typo; UART0 is what the silicon exposes.
    constexpr uintptr_t PORTB_PCR16 = mmap::PORTB_BASE + 16u * reg::port::PCR_STRIDE; // UART0_RX (ALT3)
    constexpr uintptr_t PORTB_PCR17 = mmap::PORTB_BASE + 17u * reg::port::PCR_STRIDE; // UART0_TX (ALT3)

    // FRDM-K64F onboard RGB, RED = PTB22, ACTIVE-LOW (pin low = lit).
    constexpr uintptr_t PORTB_PCR22 = mmap::PORTB_BASE + 22u * reg::port::PCR_STRIDE;
    constexpr uintptr_t GPIOB_PSOR = mmap::GPIOB_BASE + reg::gpio::PSOR_OFFSET;
    constexpr uintptr_t GPIOB_PCOR = mmap::GPIOB_BASE + reg::gpio::PCOR_OFFSET;
    constexpr uintptr_t GPIOB_PDDR = mmap::GPIOB_BASE + reg::gpio::PDDR_OFFSET;
    constexpr uint32_t LED_RED_BIT = 1u << 22;

    constexpr uintptr_t UART0_BDH = mmap::UART0_BASE + reg::uart::BDH_OFFSET;
    constexpr uintptr_t UART0_BDL = mmap::UART0_BASE + reg::uart::BDL_OFFSET;
    constexpr uintptr_t UART0_C1 = mmap::UART0_BASE + reg::uart::C1_OFFSET;
    constexpr uintptr_t UART0_C2 = mmap::UART0_BASE + reg::uart::C2_OFFSET;
    constexpr uintptr_t UART0_S1 = mmap::UART0_BASE + reg::uart::S1_OFFSET;
    constexpr uintptr_t UART0_S2 = mmap::UART0_BASE + reg::uart::S2_OFFSET;
    constexpr uintptr_t UART0_C3 = mmap::UART0_BASE + reg::uart::C3_OFFSET;
    constexpr uintptr_t UART0_D = mmap::UART0_BASE + reg::uart::D_OFFSET;
    constexpr uintptr_t UART0_C4 = mmap::UART0_BASE + reg::uart::C4_OFFSET;
    constexpr uintptr_t UART0_C5 = mmap::UART0_BASE + reg::uart::C5_OFFSET;
    constexpr uintptr_t UART0_MODEM = mmap::UART0_BASE + reg::uart::MODEM_OFFSET;
    constexpr uintptr_t UART0_IR = mmap::UART0_BASE + reg::uart::IR_OFFSET;
    constexpr uintptr_t UART0_PFIFO = mmap::UART0_BASE + reg::uart::PFIFO_OFFSET;
    constexpr uintptr_t UART0_CFIFO = mmap::UART0_BASE + reg::uart::CFIFO_OFFSET;
    constexpr uintptr_t UART0_C7816 = mmap::UART0_BASE + reg::uart::C7816_OFFSET;

    // The window arch_console_reclaim rewrites, and the one the driver is granted. ONE
    // constant: a reclaim reaching outside the window it reports would rewrite registers
    // whose holder was never checked.
    constexpr uintptr_t CONSOLE_WIN_BASE = mmap::UART0_BASE;
    constexpr size_t CONSOLE_WIN_SIZE = 0x20u;

    // Every register the reclaim body writes must lie inside that window; adding a store
    // outside it fails to build rather than silently widening the reclaim's reach.
    static_assert(UART0_BDH >= CONSOLE_WIN_BASE and UART0_BDH < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                  and UART0_BDL < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                  and UART0_C1 < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                  and UART0_C2 < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                  and UART0_C3 < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                  and UART0_C4 < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                  and UART0_C5 < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                  and UART0_S2 < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                  and UART0_IR < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                  and UART0_MODEM < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                  and UART0_PFIFO < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                  and UART0_CFIFO < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                  and UART0_C7816 < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE,
                  "arch_console_reclaim writes outside the window it reports");

    // --- PIT: the monotonic time base (K64 RM ch.44) ----------------------------
    // DWT_CYCCNT reads are unreliable on this part: it lives in the core debug power
    // domain and intermittently returns garbage (observed 0x40000001 == DWT_CTRL),
    // which the software wrap-extension turns into a phantom 2^32 clock jump that
    // strands every timed wait. Two chained 32-bit PIT channels form the free-running
    // 64-bit down-counter behind arch_clock_now. arch_trace_now and the KICKOS_BENCH
    // switch.S timestamps stay on raw DWT_CYCCNT, where a glitch costs one sample.
    // CEILING: the kernel time base (ch0/ch1) and PIT_MCR share ONE AIPS peripheral
    // slot, so a userspace PIT driver that opens that slot to U-mode (k64drv clears
    // PACR55.SP) reaches ch0/ch1 and MCR; a rogue MCR=MDIS write freezes the kernel
    // clock.
    void pit_clock_init()
    {
        // Boot-order constraint: arch_clock_now MUST NOT run before this. The PIT is
        // clock-gated out of reset and an ungated AIPS read BusFaults.
        r32(reg::sim::SCGC6) |= reg::sim::SCGC6_PIT; // clock the PIT module
        // The gate opens some bus cycles after the SCGC6 store issues, and a PIT store
        // that beats it is dropped. Read SCGC6 back so the write commits first. Without
        // this the MCR write below is lost at -Os, MCR keeps its MDIS=1 reset value, and
        // the counter never runs while the later LDVAL/TCTRL writes still land. A
        // (void) cast will not do: it performs no access on a volatile lvalue.
        uint32_t const gate = r32(reg::sim::SCGC6);
        __asm volatile("" ::"r"(gate) : "memory");
        r32(reg::pit::MCR) = 0;                      // MDIS=0 (enable), FRZ=0
        // Free-running: both channels reload from all-ones; ch1 (MSW) decrements
        // when ch0 (LSW) rolls under. Program reloads, chain ch1 to ch0, then
        // enable ch0 last so the 64-bit counter starts coherently.
        r32(reg::pit::LDVAL0) = 0xFFFFFFFFu;
        r32(reg::pit::LDVAL1) = 0xFFFFFFFFu;
        r32(reg::pit::TCTRL1) = reg::pit::TCTRL_CHN | reg::pit::TCTRL_TEN;
        r32(reg::pit::TCTRL0) = reg::pit::TCTRL_TEN;
    }

    // Elapsed PIT ticks since start (the counter runs DOWN from all-ones). Read the
    // MSW LAST and retry until it is stable across the LSW read: the final read is
    // the one that validates the pair, so a preemption straddling a ch0 roll-under
    // cannot return a torn (backward-stepped) value. Concurrency-safe with no IRQ
    // save: every load re-reads hardware, nothing is cached across the window.
    uint64_t pit_ticks()
    {
        uint32_t hi;
        uint32_t lo;
        do
        {
            hi = r32(reg::pit::CVAL1);
            lo = r32(reg::pit::CVAL0);
        } while (r32(reg::pit::CVAL1) != hi);
        uint64_t down = (static_cast<uint64_t>(hi) << 32) | lo;
        return ~down; // 0xFFFF... - down == elapsed
    }

    // arch_clock_now epoch anchor (B2, shared: kickos/arch/clk_anchor.h). Written ONLY
    // at a rate edge: init() in arch_init and reprice() in arch_cpu_clock_set. The PIT
    // is clocked by the BUS clock, not the core clock, so every rate handed to the
    // anchor is SystemCoreClock / BUS_DIV.
    kickos::arch_clk_anchor g_clk;

    void wdog_disable()
    {
        // WDOG resets the part ~238 ms after reset if left enabled, so this runs
        // first (RM 24.3.2: the unlock must also complete within 256 bus cycles
        // of reset). The two unlock keys must land within 20 bus cycles of each
        // other (RM 24.3.1), hence one asm block: an unoptimized (-O0) build could
        // otherwise insert a non-inlined store helper between them.
        volatile uint16_t* unlock = reinterpret_cast<volatile uint16_t*>(reg::wdog::UNLOCK);
        uint32_t k1 = reg::wdog::UNLOCK_KEY_1;
        uint32_t k2 = reg::wdog::UNLOCK_KEY_2;
        __asm volatile("strh %1, [%0]\n\t"
                       "strh %2, [%0]"
                       ::"r"(unlock), "r"(k1), "r"(k2) : "memory");
        // STCTRLH := reset value 0x01D3 with WDOGEN cleared, keeping ALLOWUPDATE and
        // the reset-1 reserved bit 8; a bare 0x0010 would clear that reserved bit.
        r16(reg::wdog::STCTRLH) = reg::wdog::STCTRLH_DISABLE;
    }

    bool mcg_wait(uint8_t mask, uint8_t want)
    {
        for (uint32_t i = 0; i < MCG_POLL_TIMEOUT; i++)
        {
            if ((r8(reg::mcg::S) & mask) == want)
            {
                return true;
            }
        }
        return false;
    }

    // Undo any external/PLL commit and return the MCG to the FEI reset posture that
    // SystemCoreClock still reflects. Without this a partial bring-up (EXT mux switched
    // but PLL LOCK timed out) leaves the core at 50/120 MHz while software believes
    // 20.97 MHz, with CLKS=EXT still armed so a late-arriving reference completes the
    // switch mid-run.
    // The ONE place the "return the truthful landed Hz" contract is ASSUMED rather than
    // confirmed: the two mcg_wait results below are best-effort and discarded, and the
    // caller unconditionally sets SystemCoreClock = 20971520.
    bool fail_to_fei()
    {
        r8(reg::mcg::C6) = 0;                     // clear PLLS + VDIV
        r8(reg::mcg::C5) = 0;                     // clear PRDIV
        r8(reg::mcg::C1) = reg::mcg::C1_IREFS_INT; // CLKS=0 (FLL output) + internal ref -> FEI
        mcg_wait(reg::mcg::S_IREFST, reg::mcg::S_IREFST); // best-effort: internal ref reselected
        mcg_wait(reg::mcg::S_CLKST_MASK, 0);              // CLKST=0 (FEI/FLL output)
        return false;
    }

    // FEI -> FBE -> PBE -> PEE off the FRDM's 50 MHz external clock. On any failure after
    // the external switch is requested, restores the FEI posture (fail_to_fei). Does NOT
    // touch SIM_CLKDIV1 or SystemCoreClock: the caller must widen the dividers before the
    // rise and record the landed Hz.
    bool mcg_to_pee()
    {
        // 50 MHz external clock into EXTAL0 (EREFS0=0 = bypass the crystal osc).
        r8(reg::osc::CR) = reg::osc::CR_ERCLKEN;
        r8(reg::mcg::C2) = reg::mcg::C2_RANGE_VHF;

        // FEI -> FBE: take the external reference; /1536 keeps the (PEE-unused) FLL
        // input inside its 31.25-39.0625 kHz window.
        r8(reg::mcg::C1) = reg::mcg::C1_CLKS_EXT | reg::mcg::C1_FRDIV_1536;
        if (not mcg_wait(reg::mcg::S_IREFST, 0))
        {
            return fail_to_fei();
        }
        if (not mcg_wait(reg::mcg::S_CLKST_MASK, reg::mcg::S_CLKST_EXT))
        {
            return fail_to_fei();
        }

        // FBE -> PBE: PLL ref 2.5 MHz, VCO 120 MHz. Wait for PLL-selected + lock.
        r8(reg::mcg::C5) = reg::mcg::C5_PRDIV_20;
        r8(reg::mcg::C6) = reg::mcg::C6_PLLS | reg::mcg::C6_VDIV_48;
        if (not mcg_wait(reg::mcg::S_PLLST, reg::mcg::S_PLLST))
        {
            return fail_to_fei();
        }
        if (not mcg_wait(reg::mcg::S_LOCK0, reg::mcg::S_LOCK0))
        {
            return fail_to_fei();
        }

        // PBE -> PEE: select the PLL output as MCGOUTCLK.
        r8(reg::mcg::C1) = reg::mcg::C1_CLKS_PLL | reg::mcg::C1_FRDIV_1536;
        if (not mcg_wait(reg::mcg::S_CLKST_MASK, reg::mcg::S_CLKST_PLL))
        {
            return fail_to_fei();
        }
        return true;
    }

    bool clock_init()
    {
        // Set the bus dividers BEFORE the core scales up (RM 26: widen dividers
        // first, else bus/flash overrun when MCGOUTCLK jumps to 120 MHz). Safe now
        // because we are still on the ~20.97 MHz FEI clock.
        r32(reg::sim::CLKDIV1) = CLKDIV1_120MHZ;
        if (mcg_to_pee())
        {
            SystemCoreClock = 120000000u;
            return true;
        }
        return false; // fail_to_fei parked us on FEI; SystemCoreClock stays 20.97 MHz
    }

    void uart0_init()
    {
        r32(reg::sim::SCGC5) |= reg::sim::SCGC5_PORTB; // clock PORTB
        r32(reg::sim::SCGC4) |= reg::sim::SCGC4_UART0; // clock UART0
        r32(PORTB_PCR16) = reg::port::PCR_MUX_ALT3;
        r32(PORTB_PCR17) = reg::port::PCR_MUX_ALT3;

        r8(UART0_C2) = 0; // disable TX/RX while configuring
        // baud = clk / (16 x (SBR + BRFA/32)); UART0 is system-clocked, so derive
        // SBR + the 1/32 fine-adjust from the live clock (tracks 120 MHz or the
        // 20.97 MHz FEI fallback). 20.97 MHz -> SBR 11/BRFA 12; 120 MHz -> 65/3.
        uint32_t const baud = 115200u;
        uint32_t sbr = SystemCoreClock / (16u * baud);
        uint32_t brfa = (SystemCoreClock * 2u) / baud - sbr * 32u;
        r8(UART0_BDH) = static_cast<uint8_t>((sbr >> 8) & 0x1F);
        r8(UART0_BDL) = static_cast<uint8_t>(sbr & 0xFF);
        r8(UART0_C4) = static_cast<uint8_t>(brfa & 0x1F); // BRFA fine-adjust (low 5 bits)
        r8(UART0_C2) = reg::uart::C2_TE | reg::uart::C2_RE; // TIE stays clear; the console ring primes it
    }

    // Buffered console TX backend (console_tx.h); drains on the UART0 TX-empty IRQ.
    int k64_tx_slot_free(void) { return (r8(UART0_S1) & reg::uart::S1_TDRE) != 0; }
    void k64_tx_push(uint8_t b) { r8(UART0_D) = b; }
    void k64_tx_irq_enable(void) { r8(UART0_C2) = static_cast<uint8_t>(r8(UART0_C2) | reg::uart::C2_TIE); }
    void k64_tx_irq_disable(void) { r8(UART0_C2) = static_cast<uint8_t>(r8(UART0_C2) & ~reg::uart::C2_TIE); }

    constexpr uint32_t CONSOLE_TX_SIZE = 512; // power of two; > kprintf's 256B buffer
    char console_tx_buf[CONSOLE_TX_SIZE];
    console_tx_backend const k64_console_backend = {
        k64_tx_slot_free, k64_tx_push, k64_tx_irq_enable, k64_tx_irq_disable};

    // --- SYSMPU (K64 RM section 19); base 0x4000_D000 -------------------------
    // NXP's byte/32-granular bus-master protection, NOT the ARM core MPU
    // (__MPU_PRESENT=0 here): K64F replaces the PMSAv7 commit, not the shared stash.
    // The Cortex-M4 core is TWO crossbar masters (RM 3.3.6.1): M0 = code bus
    // (instruction fetch + flash literal/rodata reads), M1 = system bus (SRAM +
    // peripheral data). RGD0 is the supervisor background; RGD1..11 are per-thread
    // USER grants. An access is allowed if ANY valid descriptor grants it (union),
    // so RGD0 (supervisor rwx everywhere) always covers privileged code. Register
    // map is in regs/sysmpu.h.
#if KICKOS_HAVE_MPU
    // WORD2 for the core's two crossbar masters (attr = the UNPRIVILEGED rights).
    // The Cortex-M4 core reaches memory as M0 (code bus) OR M1 (system bus), chosen
    // by ADDRESS: M0 serves flash AND SRAM_L (both < 0x2000_0000), M1 serves SRAM_U
    // + peripherals. A thread's stack/data can sit in EITHER SRAM bank (this chip's
    // RAM pool starts in SRAM_L @ 0x1FFF_0000), and an exception (un)stack to a
    // SRAM_L stack is an M0 data access, so granting data only on M1 denies it.
    // Grant the rights on BOTH masters: the RGD is address-bounded, so this widens
    // only the bus a thread may use, not the range it reaches. Supervisor SM left
    // 0 (=r/w/x) -> RGD0 background covers privileged; execute stays code-bus only.
    uint32_t sysmpu_word2(uint32_t attr)
    {
        uint32_t w = reg::sysmpu::WORD2_M0UM_R | reg::sysmpu::WORD2_M1UM_R; // read: M0 + M1
        if (attr & ARCH_MPU_W)
        {
            w |= reg::sysmpu::WORD2_M0UM_W | reg::sysmpu::WORD2_M1UM_W; // write: M0 + M1
        }
        if (attr & ARCH_MPU_X)
        {
            w |= reg::sysmpu::WORD2_M0UM_X; // execute: code bus (M0) only
        }
        return w;
    }
#endif
}

extern "C"
{

void arch_init(void)
{
    // FPU is enabled earlier (Reset_Handler, before C++ ctors). Raise the core to
    // 120 MHz BEFORE UART (baud) + SysTick are programmed; on failure both fall
    // back cleanly to the 20.97 MHz FEI clock (SystemCoreClock unchanged).
    clock_init();
    pit_clock_init(); // monotonic time base (see pit_clock_init note; replaces DWT)
    g_clk.init(SystemCoreClock / BUS_DIV); // anchor ONCE from the final bus clock (B2)
    uart0_init();
    kickos_armv7m_init();
}

// Monotonic clock override: convert free-running PIT ticks (bus clock = core/2) to ns,
// the required per-chip arch_clock_now (the DWT is unreliable on this silicon). Pure epoch
// read: the anchor holds the rate, so a read in the window around a retune cannot bake
// the phantom rate jump into the epoch.
uint64_t arch_clock_now(void)
{
    return g_clk.ns_from(pit_ticks());
}

// Clock-select MECHANISM (arch.h): a staged fixed set. MAX/MID land on PEE (120 MHz);
// LOW parks on the FEI internal clock (~20.97 MHz). Returns the LANDED core Hz; a
// failed relock (fail_to_fei) still MOVED the clock and returns the truthful ~20.97 MHz
// (never 0), so cpu_clock_set runs the coherence tail (B1). The generic tail re-derives
// the baud + re-arms SysTick. Called privileged, IRQs already masked. PIT is bus =
// core/BUS_DIV, so the monotonic clock's rate moves and MUST be re-anchored here.
uint32_t arch_cpu_clock_set(uint32_t target)
{
    uint32_t const previous = SystemCoreClock;
    // Achievable set is {120 MHz PEE, 20.97 MHz FEI}; MID rounds UP to MAX (no distinct
    // mid point on this staged chip). The truthful landed Hz is the return value.
    uint32_t want = 120000000u;
    if (static_cast<kos_pstate_t>(target) == KOS_PSTATE_LOW)
    {
        want = 20971520u;
    }
    if (want == previous)
    {
        return previous;
    }

    // Re-anchor capture AT the edge: history priced at the OLD mult before it moves.
    uint64_t const t0 = pit_ticks();
    uint64_t const ns0 = g_clk.ns_from(t0);

    uint32_t landed;
    if (want > previous)
    {
        // RISE (FEI -> PEE): widen the bus/flash dividers BEFORE MCGOUTCLK climbs (RM
        // 26; the flash divider /5 keeps FLASHCLK <= 25 MHz).
        r32(reg::sim::CLKDIV1) = CLKDIV1_120MHZ;
        if (mcg_to_pee())
        {
            landed = 120000000u;
        }
        else
        {
            landed = 20971520u; // relock failed -> parked on FEI (B1: truthful, tail runs)
        }
    }
    else
    {
        // FALL (PEE -> FEI): drop the MCG first (fail_to_fei walks it back to the FEI
        // reset posture). The /5 flash divider is safe at any lower core clock, so no
        // post-fall divider relax is needed (leaving it wider never faults).
        fail_to_fei();
        landed = 20971520u;
    }

    SystemCoreClock = landed;

    // Commit the NEW pricing (B2). base_ns holds history at old pricing; the staged
    // MCG walk (masked, ~1 ms) is the only mispriced interval. The caller's IrqLock
    // guards the stores against tearing, but not against a synchronous fault handler
    // reading arch_clock_now between them (panic-path-only window, accepted).
    g_clk.reprice(t0, ns0, landed / BUS_DIV);
    return landed;
}

// Clock-select console coherence (arch.h). UART0 is system-clocked, so its baud moves
// with a clock-select and must be re-derived; and no byte may be mid-shift at the old
// baud when the clock moves. Both run under the caller's IrqLock (see cpu_clock_set).
void arch_console_flush_sync(void)
{
    // Wait for the shift register to fully empty (S1.TC), not merely the data register
    // (S1.TDRE): TDRE leaves one byte still clocking out. Bounded like the sync writer.
    uint32_t spin = 0;
    while ((r8(UART0_S1) & reg::uart::S1_TC) == 0)
    {
        if (++spin > KICKOS_POLL_SPIN_MAX)
        {
            return; // a wedged UART must not hang the retune (matches the sync writer)
        }
    }
}

void arch_console_retune(void)
{
    // Re-derive SBR + the 1/32 fine-adjust from the landed SystemCoreClock (120 MHz or
    // the 20.97 MHz FEI point), as uart0_init does. The divisor write needs TX/RX
    // disabled; TIE stays clear (the console ring re-primes it on the next write).
    r8(UART0_C2) = 0;
    uint32_t const baud = 115200u;
    uint32_t const sbr = SystemCoreClock / (16u * baud);
    uint32_t const brfa = (SystemCoreClock * 2u) / baud - sbr * 32u;
    r8(UART0_BDH) = static_cast<uint8_t>((sbr >> 8) & 0x1F);
    r8(UART0_BDL) = static_cast<uint8_t>(sbr & 0xFF);
    r8(UART0_C4) = static_cast<uint8_t>(brfa & 0x1F);
    r8(UART0_C2) = reg::uart::C2_TE | reg::uart::C2_RE;
}

// Branch-clock oracle (arch.h): report the branch clock feeding a peripheral block so a
// userspace driver derives its own divisor. The K64 clock tree splits the UART tree in
// two: UART0/UART1 are system-clocked (= SystemCoreClock), while UART2/3/4 and the bus
// peripherals (DSPI0, PIT) run at the bus clock (SystemCoreClock / BUS_DIV). Read from
// the LIVE SystemCoreClock so a clock-select retune is reflected. A block this chip does
// not model returns 0. Exact block-base match: the contract only promises the base.
uint32_t arch_periph_clock_hz(uintptr_t base)
{
    if (base == mmap::UART0_BASE or base == mmap::UART1_BASE)
    {
        return SystemCoreClock; // system-clocked UARTs
    }
    if (base == mmap::UART2_BASE or base == mmap::UART3_BASE or base == mmap::UART4_BASE
        or base == mmap::DSPI0_BASE or base == mmap::PIT_BASE)
    {
        return SystemCoreClock / BUS_DIV; // bus-clocked peripherals
    }
    return 0;
}

// Peripheral-enable table (arch.h): ungate the block's SIM clock, clear its AIPS0
// Supervisor-Protect bit. Exact-base match, never a range; the SIM bit and the PACR
// register + SP bit are both derived from `base`.
//
// No PIT entry: PACR slot 55 covers the whole 4 KB block (RM 4.5.2 + 20.2.3), so
// clearing SP for a granted ch2 window (0x4003_7120) would expose the chained ch0+ch1
// pair arch_clock_now uses (RM ch.44), which arch_reserved_blocks protects by address.
// pit_clock_init already gates SCGC6_PIT at boot.
int arch_periph_enable(uintptr_t base)
{
    uint32_t scgc_bit;
    uintptr_t scgc;
    if (base == mmap::UART0_BASE)
    {
        scgc = reg::sim::SCGC4;
        scgc_bit = reg::sim::SCGC4_UART0; // RM 12.2.13
    }
    else if (base == mmap::DSPI0_BASE)
    {
        scgc = reg::sim::SCGC6;
        scgc_bit = reg::sim::SCGC6_SPI0; // RM 12.2.13
    }
    else
    {
        return -KOS_EINVAL;
    }
    uint32_t const slot = reg::aips::slot_of(base);
    if (slot == reg::aips::SLOT_NONE)
    {
        return -KOS_EINVAL; // outside AIPS0: no PACR field here, refuse rather than derive one
    }
    // Clock FIRST: a PACR-open but ungated block BusFaults on the holder's first access.
    r32(scgc) |= scgc_bit;
    r32(reg::aips::pacr_of(slot)) &= ~reg::aips::pacr_sp_bit(slot);
    return 0;
}

#if KICKOS_HAVE_MPU
// Shared pending-region stash written by the ARM-common arch_mpu_apply (stash-only).
// K64F overrides only the commit, so there is no duplicate arch_mpu_apply symbol.
extern "C" struct arch_mpu_encoded const* kickos_arm_mpu_pending(void);

// Pack the region set into the three RGD words a commit writes. SRTADDR/ENDADDR are
// addr[31:5], so a region whose base or end is not 32-byte aligned would be programmed as
// a window rounded outward from what was asked; such a region gets WORD2 0 instead, which
// the commit reads as "invalidate this descriptor".
extern "C" uint32_t arch_mpu_encode(struct arch_mpu_region const* regions, size_t n,
                                    struct arch_mpu_encoded* out)
{
    if (n > ARCH_MPU_ENCODED_SLOTS)
    {
        n = ARCH_MPU_ENCODED_SLOTS;
    }
    uint32_t seated = 0;
    size_t i = 0;
    for (; i < n; i++)
    {
        out->word0[i] = 0;
        out->word1[i] = 0;
        out->word2[i] = 0;
        if (arch_mpu_region_encodable(regions[i].base, regions[i].size))
        {
            uintptr_t const base = regions[i].base;
            out->word0[i] = static_cast<uint32_t>(base);
            out->word1[i] = static_cast<uint32_t>(base + regions[i].size - 1);
            out->word2[i] = sysmpu_word2(regions[i].attr);
            seated |= static_cast<uint32_t>(1) << i;
        }
    }
    for (; i < ARCH_MPU_ENCODED_SLOTS; i++)
    {
        out->word0[i] = 0;
        out->word1[i] = 0;
        out->word2[i] = 0;
    }
    return seated;
}

// SYSMPU commit: replaces the PMSAv7 kickos_arch_mpu_commit fallback (K64F has no ARM
// core MPU). Programs the running thread's per-thread USER grants (RGD1..) from the
// shared stash; supervisor + DMA stay covered by RGD0. Runs AFTER the physical swap.
// cpsid brackets the reprogram: PendSV is lowest-priority, so a device IRQ could
// otherwise preempt a half-written (VLD-cleared) descriptor set.
extern "C" void kickos_arch_mpu_commit(void)
{
    struct arch_mpu_encoded const* const img = kickos_arm_mpu_pending();
    if (img == nullptr)
    {
        return;
    }
    uint32_t primask;
    __asm volatile("mrs %0, primask" : "=r"(primask));
    __asm volatile("cpsid i" ::: "memory");
    // One-time: make RGD0 a supervisor-only background. At reset RGD0 grants ALL
    // masters rwx over all memory; strip the core's USER access on BOTH masters so
    // a U-mode access then needs a per-thread RGD, while supervisor (the privileged
    // kernel) keeps full rwx. RGDAAC0 is the WORD2 alt view (does not clear VLD).
    static bool rgd0_ready = false;
    if (not rgd0_ready)
    {
        // CRITICAL: RGD0 resets with the SUPERVISOR fields M0SM/M1SM = 0b11 = "same
        // as user mode" (K64 RM 19.6.1). Clearing only the user fields (M0UM/M1UM)
        // therefore drops SUPERVISOR access too, since it defers to the now-zero user
        // field: the privileged kernel faults on its very next instruction fetch,
        // double-faults while stacking, and the core locks up -> reset with no dump.
        // So ALSO clear M0SM/M1SM to 0b00 (= supervisor r/w/x), pinning supervisor
        // full-access independent of UM.
        // Bit fields (both core masters): M0UM[2:0] M0SM[4:3], M1UM[8:6] M1SM[10:9].
        constexpr uint32_t core_user_and_sm =
            reg::sysmpu::WORD2_M0UM_R | reg::sysmpu::WORD2_M0UM_W | reg::sysmpu::WORD2_M0UM_X
            | reg::sysmpu::WORD2_M0SM
            | reg::sysmpu::WORD2_M1UM_R | reg::sysmpu::WORD2_M1UM_W | reg::sysmpu::WORD2_M1UM_X
            | reg::sysmpu::WORD2_M1SM;
        r32(reg::sysmpu::RGDAAC0) &= ~core_user_and_sm;
        r32(reg::sysmpu::CESR) |= reg::sysmpu::CESR_VLD; // (already enabled at reset)
        rgd0_ready = true;
    }
    // Program RGD1..(n) from the region set; invalidate the rest. RGD0 stays the
    // background. Writing WORD2 clears VLD, so set WORD0/1/2 then WORD3=VLD last.
    for (size_t i = 0; i + 1 < reg::sysmpu::RGD_COUNT; i++)
    {
        uintptr_t const rgd = reg::sysmpu::RGD + (i + 1) * reg::sysmpu::RGD_STRIDE;
        if (i < ARCH_MPU_ENCODED_SLOTS and img->word2[i] != 0u)
        {
            r32(rgd + 0x0) = img->word0[i];                                 // SRTADDR[31:5]
            r32(rgd + 0x4) = img->word1[i];                                 // ENDADDR[31:5]
            r32(rgd + reg::sysmpu::RGD_WORD2) = img->word2[i];              // clears VLD
            r32(rgd + reg::sysmpu::RGD_WORD3) = reg::sysmpu::RGD_WORD3_VLD; // VLD=1
        }
        else
        {
            r32(rgd + reg::sysmpu::RGD_WORD3) = 0u; // invalidate the descriptor
        }
    }
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
    __asm volatile("msr primask, %0" ::"r"(primask) : "memory");
}
#endif

// SYSMPU is byte-granular on a 32-byte page (SRTADDR/ENDADDR are addr[31:5]): a window
// is exact iff base and base+size both land on a 32-byte boundary. No power-of-two size
// is needed, unlike the PMSA fallback this replaces.
bool arch_mpu_region_encodable(uintptr_t base, size_t size)
{
    if (size < 32u)
    {
        return false;
    }
    return (base & 31u) == 0 and (size & 31u) == 0;
}

// Outside KICKOS_HAVE_MPU on purpose: a no-enforcement build needs this symbol too.
int arch_mpu_region_pow2(void)
{
    return 0;
}

// A SYSMPU descriptor is access permissions only and names no memory type, but the
// Cortex-M4 on this part has no data cache.
int arch_mpu_nocache_support(void)
{
    return ARCH_MPU_NOCACHE_ALREADY;
}

#if KICKOS_HAVE_MPU
// Rule 7 reserved set (K64 RM). Granting SYSMPU or an AIPS bridge control page would be
// total escalation: SYSMPU holds the isolation regions, and one PACR SP bit opens a whole
// 4 KB peripheral slot to EVERY unprivileged thread, which SYSMPU cannot gate (see
// arch_fault_report_extra). The watchdog INSTANCE is excluded (neutralize-then-grant).
size_t arch_reserved_blocks(struct arch_reserved_block* out, size_t max)
{
    static struct arch_reserved_block const blocks[] = {
        // PIT ch2 @0x40037120 is deliberately outside the block: k64drv grants CH2, and
        // the adjacency-allowed overlap predicate lets it sit at reserved_last+1.
        {mmap::PIT_BASE, 0x120u},     // PIT: MCR + ch0 + ch1 (RM ch.44)
        {mmap::SIM_BASE, 0x1000u},    // SIM: SCGC clock-gate registers (RM ch.12)
        {mmap::MCG_BASE, 0x1000u},    // MCG: PLL / clock source (RM ch.25)
        {mmap::SYSMPU_BASE, 0x1000u}, // SYSMPU: the bus-side MPU itself (RM ch.19)
        {mmap::AIPS0_BASE, 0x1000u},  // AIPS0: MPRA + PACRA..PACRP, slots 0..127 (RM ch.20)
        {mmap::AIPS1_BASE, 0x1000u},  // AIPS1: MPRA + PACRA..PACRP, slots 128..255 (RM ch.20)
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

// K64F is a Cortex-M4 with the bit-band peripheral/SRAM alias.
int arch_bitband_present(void)
{
    return 1;
}

// Chip fault-decode hook (arch.h): a SYSMPU protection error reaches the core as a BUS
// error (escalates to HardFault; the CFSR MMFSR is 0, so the shared reporter cannot name
// it). IMPORTANT (K64 RM 3.3.6.2 / 3.3.7.1): the MPU slave ports cover flash, SRAM_L/U
// and FlexBus ONLY. The AIPS peripheral bridges and the GPIO controller are NOT slave
// ports ("protection built into the bridge"), so a peripheral-window violation does NOT
// set SPERR, and that no-SPERR case is itself the diagnostic.
// Runs privileged (RGD0 full access), so it cannot itself fault.
void arch_fault_report_extra(void)
{
    uint32_t cesr = r32(reg::sysmpu::CESR);
    uint32_t sperr = cesr >> 27; // CESR[31:27]; bit 31 -> port 0
    if (sperr == 0)
    {
        kickos::kprintf("  SYSMPU: no protection error latched (CESR=0x%x): a bus "
                        "fault outside an MPU slave port (peripheral bridge?)\n", cesr);
        return;
    }
    for (size_t port = 0; port < reg::sysmpu::SLAVE_PORTS; port++)
    {
        if ((sperr & (1u << (4 - port))) == 0)
        {
            continue;
        }
        uint32_t ear = r32(reg::sysmpu::EAR0 + port * 8u);
        uint32_t edr = r32(reg::sysmpu::EDR0 + port * 8u);
        uint32_t master = (edr >> 4) & 0xFu;
        char const* rw = "R";
        if (edr & 1u)
        {
            rw = "W";
        }
        kickos::kprintf("  SYSMPU ISOLATION FAULT: port=%u addr=0x%x master=%u %s "
                        "EDR=0x%x\n", static_cast<unsigned>(port), ear, master, rw, edr);
        // W1C this port's SPERR, but PRESERVE VLD (bit 0, plain R/W): a bare
        // `= 1u<<(31-port)` writes VLD=0 and disables the whole SYSMPU.
        r32(reg::sysmpu::CESR) = (cesr & reg::sysmpu::CESR_VLD) | (1u << (31 - port));
    }
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
        while ((r8(UART0_S1) & reg::uart::S1_TDRE) == 0)
        {
            if (++spin > KICKOS_POLL_SPIN_MAX)
            {
                return; // bounded: a wedged UART must not hang the panic path (drop)
            }
        }
        r8(UART0_D) = static_cast<uint8_t>(buf[i]);
    }
}

console_tx_backend const* arch_console_tx_backend(char** storage, uint32_t* size, int* irq_line)
{
    *storage = console_tx_buf;
    *size = CONSOLE_TX_SIZE;
    *irq_line = irq::UART0_RXTX_IRQ;
    return &k64_console_backend;
}

// UART0 (RM ch.52; AIPS0 slot 106). On k64uartirq the IRQ thread holds this grant, not
// the service thread whose death notes the console dead.
void arch_console_reclaim_window(uintptr_t* base, size_t* size)
{
    *base = CONSOLE_WIN_BASE;
    *size = CONSOLE_WIN_SIZE;
}

// Panic-path reclaim (console.cc D6): force UART0 back to a known polled-ready 8N1 TX
// channel after a userspace driver may have garbled EVERY writable register in its granted
// window. Runs with IRQs masked, privileged; MUST be idempotent + re-entrant, so it is
// straight-line ABSOLUTE stores only, NO read-modify-write: an RMW on a garbled value is
// not safe to repeat from a nested-fault re-entry.
//
// Reclaim depth = what uart0_init sets (BDH/BDL/C4/C1/C2) plus the registers init leaves
// at reset default that a hostile driver can set to cause SILENT LOSS, each cleared to 0
// below with the failure it prevents. The clock gates (SIM_SCGC4 UART0 / SCGC5 PORTB) and
// pin mux (PORTB_PCR16/17) sit in privileged peripherals OUTSIDE the UART0 window, out of
// the driver's reach.
void arch_console_reclaim(void)
{
    r8(UART0_C2) = 0;    // disable TX/RX/TIE so the driver stops; also lets the config
                         // registers below (which require TE/RE clear) be rewritten
    r8(UART0_C5) = 0;    // TDMAS/RDMAS: disable the peripheral's own DMA request enable
    r8(UART0_MODEM) = 0; // TXCTSE=0: else the bounded polled writer waits forever on an
                         // absent CTS and drops every byte (the true silent-loss case)
    r8(UART0_C3) = 0;    // TXINV=0: an inverted TX line corrupts every framed byte
    r8(UART0_S2) = 0;    // clear any driver-set line-polarity / config status bits
    r8(UART0_IR) = 0;    // IREN=0: infrared modulation on the TX pin
    r8(UART0_C7816) = 0; // ISO-7816 smartcard framing off
    r8(UART0_PFIFO) = 0; // TX/RX FIFO disabled (single-datum mode, as at reset)
    r8(UART0_CFIFO) = reg::uart::CFIFO_TXFLUSH | reg::uart::CFIFO_RXFLUSH; // flush any queued FIFO contents

    // Re-derive baud from the live SystemCoreClock (as uart0_init): 120 MHz or the
    // 20.97 MHz FEI fallback. BDH before BDL (BDL write latches the divisor).
    uint32_t const baud = 115200u;
    uint32_t const sbr = SystemCoreClock / (16u * baud);
    uint32_t const brfa = (SystemCoreClock * 2u) / baud - sbr * 32u;
    r8(UART0_BDH) = static_cast<uint8_t>((sbr >> 8) & 0x1F);
    r8(UART0_BDL) = static_cast<uint8_t>(sbr & 0xFF);
    r8(UART0_C4) = static_cast<uint8_t>(brfa & 0x1F);

    r8(UART0_C1) = 0;              // 8N1, no loops/parity/wake
    r8(UART0_C2) = reg::uart::C2_TE; // TX enable only (the polled banner needs no RX)
}

// Kernel diagnostic LED = FRDM-K64F onboard RED (PTB22), active-low.
void arch_diag_led_init(void)
{
    r32(reg::sim::SCGC5) |= reg::sim::SCGC5_PORTB; // clock PORTB (idempotent; uart0_init also sets it)
    // No gate read-back here only because uart0_init already opened PORTB earlier in
    // arch_init; with that ordering changed the PCR store below would be dropped.
    r32(PORTB_PCR22) = reg::port::PCR_MUX_GPIO;
    r32(GPIOB_PDDR) |= LED_RED_BIT; // PTB22 output
    r32(GPIOB_PSOR) = LED_RED_BIT;  // start OFF: drive high (active-low)
}

void arch_diag_led_set(int on)
{
    if (on != 0)
    {
        r32(GPIOB_PCOR) = LED_RED_BIT; // lit: drive low
    }
    else
    {
        r32(GPIOB_PSOR) = LED_RED_BIT; // off: drive high
    }
}

// Kernel-owned pins arch_pinmux_set refuses so a board map cannot dark the console
// or steal the diag LED. PTB16/17 = console RX/TX; PTB22 = diag LED.
static bool mk64f_pin_kernel_owned(uint32_t port, uint32_t pin)
{
    return port == 1u and (pin == 16u or pin == 17u or pin == 22u);
}

// One-shot pin-function config (KOS_SYS_PINMUX_SET). PORTx base = 0x40049000 + port*0x1000
// (A=0..E=4); SCGC5 port-clock bit = 1u<<(9+port); PCRn = base + pin*4. func = the raw PCR
// value (0x100 = MUX ALT1 = GPIO; 0x300 = ALT3 = the console UART). Gate the PORT clock
// FIRST (unclocked PCR access faults).
int arch_pinmux_set(uint32_t port, uint32_t pin, uint32_t func)
{
    if (port > 4u or pin > 31u)
    {
        return -KOS_EINVAL;
    }
    if (mk64f_pin_kernel_owned(port, pin))
    {
        return -KOS_EBUSY;
    }
    r32(reg::sim::SCGC5) |= (1u << (reg::sim::SCGC5_PORT_SHIFT + port)); // gate this PORT's clock (idempotent)
    // The gate needs an intervening bus transaction before the PCR store or that store is
    // dropped (same mechanism as pit_clock_init above).
    uint32_t const gate = r32(reg::sim::SCGC5);
    __asm volatile("" ::"r"(gate) : "memory");
    uintptr_t const pcr = mmap::PORTA_BASE + port * mmap::PORT_STRIDE + pin * reg::port::PCR_STRIDE;
    r32(pcr) = func;
    return 0;
}

void Reset_Handler(void)
{
    wdog_disable(); // first: the watchdog would reset the part mid-bring-up
    kickos_armv7m_enable_fpu(); // before ANY later code (the copy loops are integer, but a
                    // hard-float ABI could emit FP anywhere; CPACR-off FP faults)

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
